/* build_dwg.c — direct proto→DWG export wrapper for the floor-planner.
 *
 * Bypasses LibreDWG's DXF text reader entirely (which is O(n^2) in distinct
 * blocks and strict/buggy for valid-DXF constructs like MTEXT code 50). Reads a
 * compact, fully-controlled description file and builds the DWG model directly
 * via the dwg_add_* API, then encodes with dwg_write_file.
 *
 * Description grammar (UTF-8, whitespace/number tokens; strings are
 * length-prefixed as  <int>:<bytes>  so they may contain spaces/newlines):
 *
 *   DWGADD1
 *   VER <token>                         ; r2000 etc.
 *   UNITS <insunits> <measurement>      ; OPTIONAL — sets $INSUNITS (DXF unit
 *                                       ; code: 1=in 2=ft 4=mm 6=m …) and
 *                                       ; $MEASUREMENT (0=English 1=metric).
 *                                       ; Peeked between VER and LAYERS; omitted
 *                                       ; = keep the document default (mm/metric).
 *                                       ; Keeps the declared unit consistent with
 *                                       ; the coordinates so a re-import matches.
 *   LAYERS <n>
 *     <color> <name:lenstr>             ; n times; color = ACI int or #RRGGBB
 *                                       ; true color (DXF 420)
 *   EXTENTS minx miny maxx maxy          ; OPTIONAL — plan bounding box (CAD
 *                                        ; space). Sets EXTMIN/EXTMAX + the
 *                                        ; *Active VPORT view so CAD viewers
 *                                        ; zoom-to-fit on open. Omitted = keep
 *                                        ; libredwg's defaults (plan opens tiny).
 *   BLOCKS <nb>                          ; OPTIONAL — omitted when no spatial
 *     <name:lenstr> <ne>                 ;   filter. nb block definitions, each
 *       <TYPE> ...                       ;   holding ne entity records (same
 *                                        ;   grammar as ENTS). Referenced by
 *                                        ;   XINSERT below (resolved by name).
 *   ENTS <n>
 *     <TYPE> ...                        ; n records, each:
 *       LINE   <li> <aci> x1 y1 x2 y2
 *       PLINE  <li> <aci> <closed> <npts> (x y bulge)*npts  ; bulge=DXF 42, 0=straight
 *       CIRCLE <li> <aci> cx cy r
 *       ARC    <li> <aci> cx cy r a0deg a1deg
 *       TEXT   <li> <aci> x y h rotdeg halign valign <text:lenstr>
 *       MTEXT  <li> <aci> x y rectwidth rotdeg attach <text:lenstr>  ; attach=DXF 71 (1..9)
 *       HATCH  <li> <aci> <nloops> [<npts> (x y)*npts]*nloops
 *                                       ; solid fill; loop 0 = outer boundary,
 *                                       ; loops 1+ = islands/holes (even-odd)
 *       HATCHP <li> <aci> <name:lenstr> <ndef>
 *                [<angleDeg> bx by ox oy <ndash> <dash>*ndash]*ndef
 *                <nloops> [<npts> (x y)*npts]*nloops   ; patterned (line) fill
 *       XINSERT <li> <aci> <block:lenstr> x y sx sy rotdeg <nclip> (x y)*nclip
 *                                       ; block reference (INSERT) carrying an
 *                                       ; AutoCAD XCLIP. nclip>=3 attaches a
 *                                       ; SPATIAL_FILTER with the clip polygon
 *                                       ; (the floor outline). nclip=0 = a plain
 *                                       ; INSERT. Mirrors the DXF spatial filter.
 *       DIM    <li> <aci> x1 y1 x2 y2 tx ty meas attach rotdeg <text:lenstr>
 *                                       ; rotated/linear DIMENSION: (x1,y1)/(x2,y2)
 *                                       ; = extension-line origins, (tx,ty) =
 *                                       ; text + dim-line point, meas = DXF 42,
 *                                       ; attach = DXF 71 (1..9), rotdeg = DXF 50
 *                                       ; dim-line angle (deg), text = DXF 1
 *                                       ; override label. Standard dimstyle auto-
 *                                       ; linked; no baked graphics block.
 *   END
 *
 * li = 0-based index into LAYERS. aci = AutoCAD color index (256 = BYLAYER).
 */
#include "config.h"
#include <dwg.h>
#include <dwg_api.h>
#include "bits.h"
#include "out_dxf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.13.4"
#endif

#define DEG2RAD(x) ((x) * M_PI / 180.0)
#define MAX_LAYERS 4096
#define MAX_BLOCKS 100000

/* Skip libredwg's handle-ref dedup while bulk-building (it's O(n) per ref →
   O(n²) overall). Defined in dwg.c. Duplicate refs are valid for output. */
extern int libredwg_skip_ref_dedup;

/* ---- tiny cursor-based tokenizer over the description buffer ---- */
typedef struct { const char *p; const char *end; int err; } Cur;

static void skipws (Cur *c) {
  while (c->p < c->end && (*c->p == ' ' || *c->p == '\n' || *c->p == '\r' || *c->p == '\t'))
    c->p++;
}
/* read a whitespace-delimited token into buf (NUL-terminated); returns len */
static int tok (Cur *c, char *buf, int bufsz) {
  skipws (c);
  int n = 0;
  while (c->p < c->end && !(*c->p == ' ' || *c->p == '\n' || *c->p == '\r' || *c->p == '\t')) {
    if (n < bufsz - 1) buf[n] = *c->p;
    n++; c->p++;
  }
  if (n < bufsz) buf[n] = 0; else buf[bufsz-1] = 0;
  if (n == 0) c->err = 1;
  return n;
}
static long tok_int (Cur *c) {
  char b[64]; tok (c, b, sizeof b); return strtol (b, NULL, 10);
}
static double tok_dbl (Cur *c) {
  char b[64]; tok (c, b, sizeof b); return strtod (b, NULL);
}
/* read length-prefixed string  <int>:<bytes>  into a freshly malloc'd buffer */
static char *tok_lenstr (Cur *c) {
  skipws (c);
  long n = 0;
  while (c->p < c->end && *c->p >= '0' && *c->p <= '9') { n = n*10 + (*c->p - '0'); c->p++; }
  if (c->p >= c->end || *c->p != ':') { c->err = 1; return NULL; }
  c->p++; /* skip ':' */
  if (c->p + n > c->end) { c->err = 1; return NULL; }
  char *s = (char *) malloc (n + 1);
  if (!s) { c->err = 1; return NULL; }
  memcpy (s, c->p, n); s[n] = 0; c->p += n;
  return s;
}

static Dwg_Version_Type parse_version (const char *v) {
  if (!v) return R_2000;
  if (!strcmp (v, "r2018") || !strcmp (v, "ac1032")) return R_2018;
  if (!strcmp (v, "r2013")) return R_2013;
  if (!strcmp (v, "r2010")) return R_2010;
  if (!strcmp (v, "r2007")) return R_2007;
  if (!strcmp (v, "r2004")) return R_2004;
  if (!strcmp (v, "r2000") || !strcmp (v, "ac1015")) return R_2000;
  if (!strcmp (v, "r14") || !strcmp (v, "r2000i")) return R_14;
  return R_2000;
}

/* a color is either an ACI index (0..256) or a 24-bit RGB true color */
typedef struct { int aci; long rgb; } Color; /* rgb < 0 means "use aci" */

/* read a color token: "#RRGGBB" -> true color, else integer ACI */
static Color tok_color (Cur *c) {
  char b[32]; tok (c, b, sizeof b);
  Color col = { 256, -1 };
  if (b[0] == '#') col.rgb = strtol (b + 1, NULL, 16);
  else col.aci = (int) strtol (b, NULL, 10);
  return col;
}

/* set common layer + color on a just-created entity */
static void set_common (Dwg_Data *dwg, void *ent, Dwg_Object_Ref *layer_ref, Color col) {
  int err = 0;
  Dwg_Object *obj = dwg_ent_generic_to_object (ent, &err);
  if (err || !obj || !obj->tio.entity) return;
  if (layer_ref) obj->tio.entity->layer = layer_ref;
  if (col.rgb >= 0) {
    obj->tio.entity->color.index = 256;
    /* Entity color uses the ENC encoding (common_entity_data.spec): the RGB
       (DXF 420) is written only when color.flag bit 0x80 is set. Without it the
       true color is silently dropped and the entity falls back to BYLAYER.
       (Layer table records take a different CMC path keyed on method, so they
       don't need this.) */
    obj->tio.entity->color.flag = 0x80;
    obj->tio.entity->color.rgb = 0xc2000000u | (unsigned) (col.rgb & 0xffffff);
    obj->tio.entity->color.method = 0xc2;
  } else {
    obj->tio.entity->color.index = (BITCODE_BS) col.aci;
  }
}

/* Parse and add ONE entity record into `hdr` (model space or a block header).
   The record is  <TYPE> <li> <color> <type-specific fields...>.  Shared by the
   top-level ENTS loop and the per-block BLOCKS loop so both speak the same
   entity grammar. Returns 1 on success, 0 on clean end-of-stream (no token
   left), -1 on a parse error (also sets c->err). */
static int parse_entity (Cur *c, Dwg_Data *dwg, Dwg_Object_BLOCK_HEADER *hdr,
                         Dwg_Object_Ref **layer_refs, long nlayers) {
  char t[32];
  if (tok (c, t, sizeof t) == 0) return 0;
  int li = (int) tok_int (c);
  Color col = tok_color (c);
  Dwg_Object_Ref *lref = (li >= 0 && li < nlayers) ? layer_refs[li] : NULL;

  if (!strcmp (t, "LINE")) {
    dwg_point_3d p1, p2;
    p1.x = tok_dbl (c); p1.y = tok_dbl (c); p1.z = 0;
    p2.x = tok_dbl (c); p2.y = tok_dbl (c); p2.z = 0;
    Dwg_Entity_LINE *e = dwg_add_LINE (hdr, &p1, &p2);
    set_common (dwg, e, lref, col);
  }
  else if (!strcmp (t, "PLINE")) {
    int closed = (int) tok_int (c);
    long npts = tok_int (c);
    if (npts < 2 || npts > 1000000) { c->err = 1; return -1; }
    dwg_point_2d *pts = (dwg_point_2d *) malloc (sizeof (dwg_point_2d) * npts);
    double *bulges = (double *) malloc (sizeof (double) * npts);
    int has_bulge = 0;
    for (long k = 0; k < npts; k++) {
      pts[k].x = tok_dbl (c); pts[k].y = tok_dbl (c);
      double b = tok_dbl (c); /* trailing per-vertex bulge (DXF 42) */
      if (bulges) { bulges[k] = b; if (b != 0.0) has_bulge = 1; }
    }
    Dwg_Entity_LWPOLYLINE *pl = dwg_add_LWPOLYLINE (hdr, (int) npts, pts);
    if (pl && closed) pl->flag |= 512;
    if (pl && has_bulge) {
      pl->bulges = bulges;
      pl->num_bulges = (BITCODE_BL) npts;
      pl->flag |= 16;
      bulges = NULL; /* ownership transferred to the entity */
    }
    set_common (dwg, pl, lref, col);
    free (pts);
    free (bulges); /* NULL after ownership transfer; else our temp */
  }
  else if (!strcmp (t, "HATCH")) {
    long nloops = tok_int (c);
    if (nloops < 1 || nloops > 100000) { c->err = 1; return -1; }
    const Dwg_Object **objs =
        (const Dwg_Object **) malloc (sizeof (Dwg_Object *) * nloops);
    unsigned nobjs = 0;
    for (long lp = 0; lp < nloops && !c->err; lp++) {
      long npts = tok_int (c);
      if (npts < 3 || npts > 1000000) { c->err = 1; break; }
      dwg_point_2d *pts = (dwg_point_2d *) malloc (sizeof (dwg_point_2d) * npts);
      for (long k = 0; k < npts; k++) { pts[k].x = tok_dbl (c); pts[k].y = tok_dbl (c); }
      Dwg_Entity_LWPOLYLINE *pl = dwg_add_LWPOLYLINE (hdr, (int) npts, pts);
      free (pts);
      if (pl) {
        pl->flag |= 512; /* closed boundary loop */
        set_common (dwg, pl, lref, col);
        int err = 0;
        const Dwg_Object *o = dwg_obj_generic_to_object (pl, &err);
        if (!err && o && objs) { objs[nobjs++] = o; }
      }
    }
    if (!c->err && objs && nobjs > 0) {
      Dwg_Entity_HATCH *h = dwg_add_HATCH (hdr, 1, "SOLID", false, nobjs, objs);
      if (h) {
        h->style = 0; /* normal (even-odd): inner loops render as holes */
        set_common (dwg, h, lref, col);
      }
    }
    free (objs);
  }
  else if (!strcmp (t, "HATCHP")) {
    char *pname = tok_lenstr (c);
    long ndef = tok_int (c);
    if (ndef < 0 || ndef > 10000) { c->err = 1; free (pname); return -1; }
    Dwg_HATCH_DefLine *defs = (Dwg_HATCH_DefLine *)
        calloc (ndef > 0 ? ndef : 1, sizeof (Dwg_HATCH_DefLine));
    for (long d = 0; d < ndef && !c->err; d++) {
      defs[d].angle = DEG2RAD (tok_dbl (c));
      defs[d].pt0.x = tok_dbl (c); defs[d].pt0.y = tok_dbl (c);
      defs[d].offset.x = tok_dbl (c); defs[d].offset.y = tok_dbl (c);
      long nd = tok_int (c);
      if (nd < 0 || nd > 1000) { c->err = 1; break; }
      defs[d].num_dashes = (BITCODE_BS) nd;
      if (nd > 0) {
        defs[d].dashes = (double *) malloc (sizeof (double) * nd);
        for (long k = 0; k < nd; k++) { defs[d].dashes[k] = tok_dbl (c); }
      }
    }
    long nloops = c->err ? 0 : tok_int (c);
    if (!c->err && (nloops < 1 || nloops > 100000)) { c->err = 1; }
    const Dwg_Object **objs = (const Dwg_Object **)
        malloc (sizeof (Dwg_Object *) * (nloops > 0 ? nloops : 1));
    unsigned nobjs = 0;
    for (long lp = 0; lp < nloops && !c->err; lp++) {
      long npts = tok_int (c);
      if (npts < 3 || npts > 1000000) { c->err = 1; break; }
      dwg_point_2d *pts = (dwg_point_2d *) malloc (sizeof (dwg_point_2d) * npts);
      for (long k = 0; k < npts; k++) { pts[k].x = tok_dbl (c); pts[k].y = tok_dbl (c); }
      Dwg_Entity_LWPOLYLINE *pl = dwg_add_LWPOLYLINE (hdr, (int) npts, pts);
      free (pts);
      if (pl) {
        pl->flag |= 512;
        set_common (dwg, pl, lref, col);
        int err = 0;
        const Dwg_Object *o = dwg_obj_generic_to_object (pl, &err);
        if (!err && o && objs) { objs[nobjs++] = o; }
      }
    }
    if (!c->err && objs && nobjs > 0) {
      Dwg_Entity_HATCH *h = dwg_add_HATCH (
          hdr, 1, (pname && *pname) ? pname : "ANSI31", false, nobjs, objs);
      if (h) {
        if (ndef > 0) {
          h->num_deflines = (BITCODE_BS) ndef;
          h->deflines = defs;
          for (long d = 0; d < ndef; d++) { h->deflines[d].parent = h; }
          defs = NULL; /* ownership transferred to the entity */
        }
        set_common (dwg, h, lref, col);
      }
    }
    if (defs) { for (long d = 0; d < ndef; d++) { free (defs[d].dashes); } free (defs); }
    free (objs);
    free (pname);
  }
  else if (!strcmp (t, "CIRCLE")) {
    dwg_point_3d ctr; ctr.x = tok_dbl (c); ctr.y = tok_dbl (c); ctr.z = 0;
    double r = tok_dbl (c);
    Dwg_Entity_CIRCLE *e = dwg_add_CIRCLE (hdr, &ctr, r);
    set_common (dwg, e, lref, col);
  }
  else if (!strcmp (t, "ARC")) {
    dwg_point_3d ctr; ctr.x = tok_dbl (c); ctr.y = tok_dbl (c); ctr.z = 0;
    double r = tok_dbl (c), a0 = tok_dbl (c), a1 = tok_dbl (c);
    Dwg_Entity_ARC *e = dwg_add_ARC (hdr, &ctr, r, DEG2RAD (a0), DEG2RAD (a1));
    set_common (dwg, e, lref, col);
  }
  else if (!strcmp (t, "TEXT")) {
    dwg_point_3d ins; ins.x = tok_dbl (c); ins.y = tok_dbl (c); ins.z = 0;
    double h = tok_dbl (c), rot = tok_dbl (c);
    int ha = (int) tok_int (c), va = (int) tok_int (c);
    char *s = tok_lenstr (c);
    Dwg_Entity_TEXT *e = dwg_add_TEXT (hdr, s ? s : "", &ins, h);
    free (s);
    if (e) {
      e->width_factor = 1.0;
      e->rotation = DEG2RAD (rot);
      e->horiz_alignment = (BITCODE_BS) ha;
      e->vert_alignment = (BITCODE_BS) va;
      if (ha || va) { e->alignment_pt.x = ins.x; e->alignment_pt.y = ins.y; }
    }
    set_common (dwg, e, lref, col);
  }
  else if (!strcmp (t, "MTEXT")) {
    dwg_point_3d ins; ins.x = tok_dbl (c); ins.y = tok_dbl (c); ins.z = 0;
    double h = tok_dbl (c), w = tok_dbl (c), rot = tok_dbl (c);
    int att = (int) tok_int (c);
    char *s = tok_lenstr (c);
    Dwg_Entity_MTEXT *e = dwg_add_MTEXT (hdr, &ins, w, s ? s : "");
    free (s);
    if (e) {
      if (h > 0) e->text_height = h;
      if (att >= 1 && att <= 9) e->attachment = (BITCODE_BS) att;
      e->x_axis_dir.x = cos (DEG2RAD (rot));
      e->x_axis_dir.y = sin (DEG2RAD (rot));
      e->x_axis_dir.z = 0;
    }
    set_common (dwg, e, lref, col);
  }
  else if (!strcmp (t, "DIM")) {
    dwg_point_3d x1, x2, tm;
    x1.x = tok_dbl (c); x1.y = tok_dbl (c); x1.z = 0;
    x2.x = tok_dbl (c); x2.y = tok_dbl (c); x2.z = 0;
    tm.x = tok_dbl (c); tm.y = tok_dbl (c); tm.z = 0;
    double meas = tok_dbl (c);
    int att = (int) tok_int (c);
    double rot = DEG2RAD (tok_dbl (c));
    while (rot > M_PI) rot -= 2.0 * M_PI;
    while (rot < -M_PI) rot += 2.0 * M_PI;
    char *s = tok_lenstr (c);
    Dwg_Entity_DIMENSION_LINEAR *e
        = dwg_add_DIMENSION_LINEAR (hdr, &x1, &x2, &tm, rot);
    if (e) {
      e->text_midpt.x = tm.x; e->text_midpt.y = tm.y;
      e->act_measurement = meas;
      if (att >= 1 && att <= 9) e->attachment = (BITCODE_BS) att;
      if (s && *s) e->user_text = strdup (s);
    }
    free (s);
    set_common (dwg, e, lref, col);
  }
  else if (!strcmp (t, "XINSERT")) {
    /* XINSERT <li> <color> <block:lenstr> x y sx sy rotdeg <nclip> (x y)*nclip
       A block reference (INSERT) at model-space level that carries an AutoCAD
       XCLIP (SPATIAL_FILTER). nclip>=3 attaches the clip polygon. The floor
       exporter uses this to clip a per-layer FLOOR_CLIP_* block to the floor
       outline — the direct-DWG mirror of the DXF INSERT-with-SPATIAL_FILTER. */
    char *bname = tok_lenstr (c);
    dwg_point_3d ins; ins.x = tok_dbl (c); ins.y = tok_dbl (c); ins.z = 0;
    double sx = tok_dbl (c), sy = tok_dbl (c), rotdeg = tok_dbl (c);
    long nclip = tok_int (c);
    if (nclip < 0 || nclip > 1000000) { c->err = 1; free (bname); return -1; }
    dwg_point_2d *cv = NULL;
    if (nclip > 0) {
      cv = (dwg_point_2d *) malloc (sizeof (dwg_point_2d) * nclip);
      for (long k = 0; k < nclip; k++) { cv[k].x = tok_dbl (c); cv[k].y = tok_dbl (c); }
    }
    if (!c->err && bname && *bname) {
      Dwg_Entity_INSERT *ie = dwg_add_INSERT (
          hdr, &ins, bname, sx != 0.0 ? sx : 1.0, sy != 0.0 ? sy : 1.0, 1.0,
          DEG2RAD (rotdeg));
      set_common (dwg, ie, lref, col);
      if (ie && cv && nclip >= 3) {
        /* dwg_add_SPATIAL_FILTER builds the INSERT's ACAD_XDICTIONARY ->
           ACAD_FILTER -> SPATIAL chain and an identity transform; we fill in
           the clip polygon + world-aligned plane it leaves empty. */
        Dwg_Object_SPATIAL_FILTER *sf = dwg_add_SPATIAL_FILTER (ie);
        if (sf) {
          sf->clip_verts = (BITCODE_2RD *) calloc ((size_t) nclip, sizeof (BITCODE_2RD));
          if (sf->clip_verts) {
            for (long k = 0; k < nclip; k++) {
              sf->clip_verts[k].x = cv[k].x; sf->clip_verts[k].y = cv[k].y;
            }
            sf->num_clip_verts = (BITCODE_BS) nclip;
          }
          sf->extrusion.x = 0.0; sf->extrusion.y = 0.0; sf->extrusion.z = 1.0;
          sf->origin.x = 0.0; sf->origin.y = 0.0; sf->origin.z = 0.0;
          sf->display_boundary_on = 1;
          sf->front_clip_on = 0;
          sf->back_clip_on = 0;
        }
      }
    }
    free (cv);
    free (bname);
  }
  else {
    /* unknown record type — bail rather than desync the stream */
    c->err = 1;
    return -1;
  }
  return c->err ? -1 : 1;
}

/* Frame the drawing on a 2D bounding box so CAD viewers zoom-to-fit on open.
   dwg_new_Document seeds EXTMIN/EXTMAX with the empty sentinel (+/-1e20) and the
   *Active VPORT with a fixed view unrelated to the model, so a freshly built DWG
   opens with the plan tiny/off-screen. We overwrite both: EXTMIN/EXTMAX (and
   LIMMIN/LIMMAX) stay exact for viewers that zoom-to-extents, while the active
   VPORT view is centered on the bbox and sized to the LARGER plan dimension
   (+10% margin) so the whole plan stays visible whatever the viewer window's
   aspect ratio. */
static void apply_extents (Dwg_Data *dwg, double minx, double miny,
                           double maxx, double maxy) {
  if (!(maxx >= minx) || !(maxy >= miny)) return; /* degenerate / NaN guard */
  double cx = (minx + maxx) / 2.0;
  double cy = (miny + maxy) / 2.0;
  double w = maxx - minx; if (!(w > 0)) w = 1.0;
  double h = maxy - miny; if (!(h > 0)) h = 1.0;
  double view = (w > h ? w : h) * 1.1; /* max dimension + margin = guaranteed fit */

  dwg->header_vars.EXTMIN.x = minx; dwg->header_vars.EXTMIN.y = miny; dwg->header_vars.EXTMIN.z = 0.0;
  dwg->header_vars.EXTMAX.x = maxx; dwg->header_vars.EXTMAX.y = maxy; dwg->header_vars.EXTMAX.z = 0.0;
  dwg->header_vars.LIMMIN.x = minx; dwg->header_vars.LIMMIN.y = miny;
  dwg->header_vars.LIMMAX.x = maxx; dwg->header_vars.LIMMAX.y = maxy;
  dwg->header_vars.VIEWCTR.x = cx; dwg->header_vars.VIEWCTR.y = cy; dwg->header_vars.VIEWCTR.z = 0.0;
  dwg->header_vars.VIEWSIZE = view;

  /* A fresh document carries exactly one VPORT (*Active). Frame it on the plan
     so viewers that restore the saved view (rather than zoom-to-extents) also
     open centered. view_width == VIEWSIZE (aspect 1.0) keeps the framing square
     so no edge is clipped regardless of the viewer's window proportions. */
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++) {
    Dwg_Object *obj = &dwg->object[i];
    if (obj->fixedtype != DWG_TYPE_VPORT) continue;
    if (!obj->tio.object || !obj->tio.object->tio.VPORT) continue;
    Dwg_Object_VPORT *vp = obj->tio.object->tio.VPORT;
    vp->VIEWCTR.x = cx;
    vp->VIEWCTR.y = cy;
    vp->VIEWSIZE = view;
    vp->view_width = view;
    vp->aspect_ratio = 1.0;
  }
}

int build_dwg (const char *in_path, const char *out_path, const char *version) {
  /* read description file */
  FILE *f = fopen (in_path, "rb");
  if (!f) return 1;
  fseek (f, 0, SEEK_END); long sz = ftell (f); fseek (f, 0, SEEK_SET);
  char *buf = (char *) malloc (sz + 1);
  if (!buf) { fclose (f); return 2; }
  if (fread (buf, 1, sz, f) != (size_t) sz) { fclose (f); free (buf); return 3; }
  buf[sz] = 0; fclose (f);

  Cur c = { buf, buf + sz, 0 };
  char kw[64];

  tok (&c, kw, sizeof kw);
  if (strcmp (kw, "DWGADD1")) { free (buf); return 10; }
  tok (&c, kw, sizeof kw);                 /* VER */
  char vbuf[32]; tok (&c, vbuf, sizeof vbuf);
  Dwg_Version_Type ver = version && *version ? parse_version (version) : parse_version (vbuf);

  Dwg_Data *dwg = dwg_new_Document (ver, 0 /*metric*/, 0 /*loglevel*/);
  if (!dwg) { free (buf); return 11; }
  libredwg_skip_ref_dedup = 1; /* near-linear bulk build */
  Dwg_Object *mspace = dwg_model_space_object (dwg);
  if (!mspace) { free (buf); return 12; }
  Dwg_Object_BLOCK_HEADER *hdr = mspace->tio.object->tio.BLOCK_HEADER;

  /* dwg_add_LAYER leaves the layer's linetype (DXF 6) unset, so every layer
     ends up with a dangling/empty linetype handle. LibreDWG tolerates that, but
     strict CAD readers (AutoCAD etc.) crash on it. Resolve the standard
     CONTINUOUS linetype once and link every layer to it below. */
  BITCODE_H cont_ltype = dwg_find_tablehandle (dwg, (char *)"CONTINUOUS", (char *)"LTYPE");
  if (!cont_ltype) cont_ltype = dwg_find_tablehandle (dwg, (char *)"Continuous", (char *)"LTYPE");
  if (!cont_ltype) cont_ltype = dwg_find_tablehandle (dwg, (char *)"ByLayer", (char *)"LTYPE");
  BITCODE_RLL cont_ltype_ref = cont_ltype ? cont_ltype->absolute_ref : 0;

  /* ---- units (optional) ----
     dwg_new_Document seeded $INSUNITS=4 (mm) + $MEASUREMENT=1 (metric) above.
     Override both from the export's UNITS record so the declared unit matches the
     unit the coordinates are written in (otherwise feet/inch geometry shipped
     under an mm header and re-imports drifted ~25–305×). The keyword is peeked, so
     a stream without UNITS streams VER -> LAYERS exactly as before. */
  tok (&c, kw, sizeof kw);                 /* "UNITS" or "LAYERS" */
  if (!c.err && !strcmp (kw, "UNITS")) {
    long insunits = tok_int (&c);
    long measurement = tok_int (&c);
    if (insunits >= 0 && insunits <= 20)
      dwg->header_vars.INSUNITS = (BITCODE_BS) insunits;
    dwg->header_vars.MEASUREMENT = (BITCODE_BS) (measurement ? 1 : 0);
    tok (&c, kw, sizeof kw);               /* now "LAYERS" */
  }

  /* ---- layers ---- */
  if (strcmp (kw, "LAYERS")) { free (buf); return 13; }
  long nlayers = tok_int (&c);
  if (nlayers < 0 || nlayers > MAX_LAYERS) { free (buf); return 13; }
  Dwg_Object_Ref **layer_refs = (Dwg_Object_Ref **) calloc (nlayers > 0 ? nlayers : 1, sizeof (void *));
  for (long i = 0; i < nlayers; i++) {
    Color col = tok_color (&c);
    char *name = tok_lenstr (&c);
    if (c.err) { free (name); break; }
    const char *lname = name && *name ? name : "0";

    /* dwg_new_Document already creates the default layer "0". Adding it (or any
       pre-existing layer) again produces a duplicate table record at the wrong
       index — AutoCAD audits it as "Layer zero name found at incorrect index"
       and strict readers can choke. Reuse an existing layer instead of adding a
       duplicate. */
    BITCODE_H existing = dwg_find_tablehandle (dwg, (char *)lname, (char *)"LAYER");
    if (existing && existing->absolute_ref) {
      layer_refs[i] = dwg_add_handleref (dwg, 5, existing->absolute_ref, NULL);
      free (name);
      continue;
    }

    Dwg_Object_LAYER *layer = dwg_add_LAYER (dwg, lname);
    free (name);
    if (layer) {
      if (col.rgb >= 0) {
        /* true color (DXF 420): keep ACI 7 as the fallback for readers that
           ignore true color, and set the RGB exactly as set_common does. */
        layer->color.index = 7;
        layer->color.rgb = 0xc2000000u | (unsigned) (col.rgb & 0xffffff);
        layer->color.method = 0xc2;
      } else {
        layer->color.index = (BITCODE_BS) (col.aci > 0 && col.aci < 256 ? col.aci : 7);
      }
      if (cont_ltype_ref) /* avoid the dangling-linetype crash in strict readers */
        layer->ltype = dwg_add_handleref (dwg, 5, cont_ltype_ref, NULL);
      int err = 0;
      Dwg_Object *lobj = dwg_obj_generic_to_object (layer, &err);
      if (!err && lobj)
        layer_refs[i] = dwg_add_handleref (dwg, 5, lobj->handle.value, NULL);
    }
  }

  /* ---- blocks (optional) ----
     A FLOOR_CLIP_<LAYER> block per layer when the floor carries a spatial
     filter; each is referenced below by an XINSERT that carries the XCLIP.
     The section is optional, so non-clip exports stream LAYERS -> ENTS with a
     description byte-identical to before. dwg_add_INSERT later resolves blocks
     by name, so they must be defined before the entities that reference them
     (which the BLOCKS-before-ENTS ordering guarantees). */
  /* ---- extents (optional) ----
     Sets the drawing extents + active viewport so CAD viewers frame the plan on
     open (see apply_extents). The keyword is peeked, so exports without a bbox
     stream LAYERS -> BLOCKS/ENTS exactly as before. */
  int have_ext = 0;
  double ext_minx = 0, ext_miny = 0, ext_maxx = 0, ext_maxy = 0;
  tok (&c, kw, sizeof kw);                 /* "EXTENTS", "BLOCKS", or "ENTS" */
  if (!c.err && !strcmp (kw, "EXTENTS")) {
    ext_minx = tok_dbl (&c); ext_miny = tok_dbl (&c);
    ext_maxx = tok_dbl (&c); ext_maxy = tok_dbl (&c);
    have_ext = 1;
    tok (&c, kw, sizeof kw);               /* now "BLOCKS" or "ENTS" */
  }

  if (!c.err && !strcmp (kw, "BLOCKS")) {
    long nblocks = tok_int (&c);
    if (nblocks < 0 || nblocks > MAX_BLOCKS) { c.err = 1; }
    for (long b = 0; b < nblocks && !c.err; b++) {
      char *bname = tok_lenstr (&c);
      long bents = tok_int (&c);
      if (c.err || !bname || !*bname) { free (bname); c.err = 1; break; }
      Dwg_Object_BLOCK_HEADER *bhdr = dwg_add_BLOCK_HEADER (dwg, bname);
      if (bhdr) { dwg_add_BLOCK (bhdr, bname); }
      for (long e = 0; e < bents && !c.err; e++) {
        if (parse_entity (&c, dwg, bhdr ? bhdr : hdr, layer_refs, nlayers) <= 0) { break; }
      }
      if (bhdr) { dwg_add_ENDBLK (bhdr); }
      free (bname);
    }
    tok (&c, kw, sizeof kw);               /* now the "ENTS" keyword */
  }

  /* ---- entities ----
     kw holds the "ENTS" keyword (read just above, with or without a preceding
     BLOCKS section). XINSERT records here reference the FLOOR_CLIP_* blocks. */
  long nents = tok_int (&c);
  for (long i = 0; i < nents && !c.err; i++) {
    if (parse_entity (&c, dwg, hdr, layer_refs, nlayers) <= 0) { break; }
  }

  int parse_err = c.err;
  free (layer_refs);
  free (buf);
  if (parse_err) { libredwg_skip_ref_dedup = 0; dwg_free (dwg); return 20; }

  if (have_ext) { apply_extents (dwg, ext_minx, ext_miny, ext_maxx, ext_maxy); }

  int werr = dwg_write_file (out_path, dwg);
  libredwg_skip_ref_dedup = 0;
  dwg_free (dwg);
  return werr; /* 0 on success */
}

/* ---- compatibility wrappers (superset of the original WASM) ---------------
 * Reconstructed from programs/dwg2dxf.c and programs/dxf2dwg.c so the rebuilt
 * module is a drop-in replacement: dwg2dxf keeps the working DWG import path,
 * dxf2dwg stays as a fallback export path, version for capability probes. */

const char *libredwg_version (void) { return PACKAGE_VERSION; }

int libredwg_dwg2dxf (const char *in_path, const char *out_path,
                      const char *opts_unused, int minimal, int flags_unused) {
  (void) opts_unused; (void) flags_unused;
  Dwg_Data dwg;
  Bit_Chain dat;
  int error;
  memset (&dwg, 0, sizeof (Dwg_Data));
  memset (&dat, 0, sizeof (Bit_Chain));
  error = dwg_read_file (in_path, &dwg);
  if (error >= DWG_ERR_CRITICAL) { dwg_free (&dwg); return error; }
  dat.version = dwg.header.version;
  dat.from_version = dwg.header.from_version;
  if (minimal) dwg.opts |= DWG_OPTS_MINIMAL;
  dat.fh = fopen (out_path, "wb");
  if (!dat.fh) { dwg_free (&dwg); return DWG_ERR_IOERROR; }
  error = dwg_write_dxf (&dat, &dwg);
  fclose (dat.fh);
  dwg_free (&dwg);
  return error;
}

int libredwg_dxf2dwg (const char *in_path, const char *out_path,
                      const char *version) {
  Dwg_Data dwg;
  int error;
  memset (&dwg, 0, sizeof (Dwg_Data));
  error = dxf_read_file (in_path, &dwg);
  if (error >= DWG_ERR_CRITICAL) { dwg_free (&dwg); return error; }
  if (version && *version) {
    Dwg_Version_Type v = parse_version (version);
    if (dwg.header.from_version != dwg.header.version)
      dwg.header.from_version = dwg.header.version;
    dwg.header.version = v;
  }
  error = dwg_write_file (out_path, &dwg);
  dwg_free (&dwg);
  return error;
}
