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
 *   LAYERS <n>
 *     <color> <name:lenstr>             ; n times; color = ACI int or #RRGGBB
 *                                       ; true color (DXF 420)
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
 *       DIM    <li> <aci> x1 y1 x2 y2 tx ty meas attach <text:lenstr>
 *                                       ; aligned DIMENSION: (x1,y1)/(x2,y2) =
 *                                       ; extension-line origins, (tx,ty) =
 *                                       ; text + dim-line point, meas = DXF 42,
 *                                       ; attach = DXF 71 (1..9), text = DXF 1
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

  /* ---- layers ---- */
  tok (&c, kw, sizeof kw);                 /* LAYERS */
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

  /* ---- entities ---- */
  tok (&c, kw, sizeof kw);                 /* ENTS */
  long nents = tok_int (&c);
  for (long i = 0; i < nents && !c.err; i++) {
    char t[32];
    if (tok (&c, t, sizeof t) == 0) break;
    int li = (int) tok_int (&c);
    Color col = tok_color (&c);
    Dwg_Object_Ref *lref = (li >= 0 && li < nlayers) ? layer_refs[li] : NULL;

    if (!strcmp (t, "LINE")) {
      dwg_point_3d p1, p2;
      p1.x = tok_dbl (&c); p1.y = tok_dbl (&c); p1.z = 0;
      p2.x = tok_dbl (&c); p2.y = tok_dbl (&c); p2.z = 0;
      Dwg_Entity_LINE *e = dwg_add_LINE (hdr, &p1, &p2);
      set_common (dwg, e, lref, col);
    }
    else if (!strcmp (t, "PLINE")) {
      int closed = (int) tok_int (&c);
      long npts = tok_int (&c);
      if (npts < 2 || npts > 1000000) { c.err = 1; break; }
      dwg_point_2d *pts = (dwg_point_2d *) malloc (sizeof (dwg_point_2d) * npts);
      double *bulges = (double *) malloc (sizeof (double) * npts);
      int has_bulge = 0;
      for (long k = 0; k < npts; k++) {
        pts[k].x = tok_dbl (&c); pts[k].y = tok_dbl (&c);
        double b = tok_dbl (&c); /* trailing per-vertex bulge (DXF 42) */
        if (bulges) { bulges[k] = b; if (b != 0.0) has_bulge = 1; }
      }
      Dwg_Entity_LWPOLYLINE *pl = dwg_add_LWPOLYLINE (hdr, (int) npts, pts);
      if (pl && closed) pl->flag |= 512;
      if (pl && has_bulge) {
        /* Attach the bulge vector so curved segments (rounded corners, arcs)
           survive instead of flattening to chords. flag 16 = bulges present;
           dwg_free releases pl->bulges with the entity. */
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
      /* HATCH <li> <color> <nloops> [<npts> (x y)*npts]*nloops
         loop 0 = outer boundary; loops 1+ = islands (holes, e.g. the counters
         of letters) — left UNFILLED by the normal (even-odd) hatch style so
         filled glyphs keep their open inner regions. */
      long nloops = tok_int (&c);
      if (nloops < 1 || nloops > 100000) { c.err = 1; break; }
      const Dwg_Object **objs =
          (const Dwg_Object **) malloc (sizeof (Dwg_Object *) * nloops);
      unsigned nobjs = 0;
      for (long lp = 0; lp < nloops && !c.err; lp++) {
        long npts = tok_int (&c);
        if (npts < 3 || npts > 1000000) { c.err = 1; break; }
        dwg_point_2d *pts = (dwg_point_2d *) malloc (sizeof (dwg_point_2d) * npts);
        for (long k = 0; k < npts; k++) { pts[k].x = tok_dbl (&c); pts[k].y = tok_dbl (&c); }
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
      if (!c.err && objs && nobjs > 0) {
        Dwg_Entity_HATCH *h = dwg_add_HATCH (hdr, 1, "SOLID", false, nobjs, objs);
        if (h) {
          h->style = 0; /* normal (even-odd): inner loops render as holes */
          set_common (dwg, h, lref, col);
        }
      }
      free (objs);
    }
    else if (!strcmp (t, "HATCHP")) {
      /* HATCHP <li> <color> <name:lenstr> <ndef>
       *          [<angleDeg> bx by ox oy <ndash> <dash>*ndash]*ndef
       *          <nloops> [<npts> (x y)*npts]*nloops
       * A patterned (non-solid) hatch: the deflines carry the pattern's line
       * family (angle, base, offset, dashes) so the fill renders as the actual
       * line pattern (e.g. diagonal hatching) instead of solid or empty. */
      char *pname = tok_lenstr (&c);
      long ndef = tok_int (&c);
      if (ndef < 0 || ndef > 10000) { c.err = 1; free (pname); break; }
      Dwg_HATCH_DefLine *defs = (Dwg_HATCH_DefLine *)
          calloc (ndef > 0 ? ndef : 1, sizeof (Dwg_HATCH_DefLine));
      for (long d = 0; d < ndef && !c.err; d++) {
        defs[d].angle = DEG2RAD (tok_dbl (&c));
        defs[d].pt0.x = tok_dbl (&c); defs[d].pt0.y = tok_dbl (&c);
        defs[d].offset.x = tok_dbl (&c); defs[d].offset.y = tok_dbl (&c);
        long nd = tok_int (&c);
        if (nd < 0 || nd > 1000) { c.err = 1; break; }
        defs[d].num_dashes = (BITCODE_BS) nd;
        if (nd > 0) {
          defs[d].dashes = (double *) malloc (sizeof (double) * nd);
          for (long k = 0; k < nd; k++) { defs[d].dashes[k] = tok_dbl (&c); }
        }
      }
      long nloops = c.err ? 0 : tok_int (&c);
      if (!c.err && (nloops < 1 || nloops > 100000)) { c.err = 1; }
      const Dwg_Object **objs = (const Dwg_Object **)
          malloc (sizeof (Dwg_Object *) * (nloops > 0 ? nloops : 1));
      unsigned nobjs = 0;
      for (long lp = 0; lp < nloops && !c.err; lp++) {
        long npts = tok_int (&c);
        if (npts < 3 || npts > 1000000) { c.err = 1; break; }
        dwg_point_2d *pts = (dwg_point_2d *) malloc (sizeof (dwg_point_2d) * npts);
        for (long k = 0; k < npts; k++) { pts[k].x = tok_dbl (&c); pts[k].y = tok_dbl (&c); }
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
      if (!c.err && objs && nobjs > 0) {
        /* non-"SOLID" name => patterned hatch (is_solid_fill stays 0) */
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
      dwg_point_3d ctr; ctr.x = tok_dbl (&c); ctr.y = tok_dbl (&c); ctr.z = 0;
      double r = tok_dbl (&c);
      Dwg_Entity_CIRCLE *e = dwg_add_CIRCLE (hdr, &ctr, r);
      set_common (dwg, e, lref, col);
    }
    else if (!strcmp (t, "ARC")) {
      dwg_point_3d ctr; ctr.x = tok_dbl (&c); ctr.y = tok_dbl (&c); ctr.z = 0;
      double r = tok_dbl (&c), a0 = tok_dbl (&c), a1 = tok_dbl (&c);
      Dwg_Entity_ARC *e = dwg_add_ARC (hdr, &ctr, r, DEG2RAD (a0), DEG2RAD (a1));
      set_common (dwg, e, lref, col);
    }
    else if (!strcmp (t, "TEXT")) {
      dwg_point_3d ins; ins.x = tok_dbl (&c); ins.y = tok_dbl (&c); ins.z = 0;
      double h = tok_dbl (&c), rot = tok_dbl (&c);
      int ha = (int) tok_int (&c), va = (int) tok_int (&c);
      char *s = tok_lenstr (&c);
      Dwg_Entity_TEXT *e = dwg_add_TEXT (hdr, s ? s : "", &ins, h);
      free (s);
      if (e) {
        /* dwg_add_TEXT leaves width_factor (DXF 41) at 0, which AutoCAD audits
           as "width not in range" (zero-width text). 1.0 is the normal value. */
        e->width_factor = 1.0;
        e->rotation = DEG2RAD (rot);
        e->horiz_alignment = (BITCODE_BS) ha;
        e->vert_alignment = (BITCODE_BS) va;
        if (ha || va) { e->alignment_pt.x = ins.x; e->alignment_pt.y = ins.y; }
      }
      set_common (dwg, e, lref, col);
    }
    else if (!strcmp (t, "MTEXT")) {
      dwg_point_3d ins; ins.x = tok_dbl (&c); ins.y = tok_dbl (&c); ins.z = 0;
      double h = tok_dbl (&c), w = tok_dbl (&c), rot = tok_dbl (&c);
      int att = (int) tok_int (&c);
      char *s = tok_lenstr (&c);
      Dwg_Entity_MTEXT *e = dwg_add_MTEXT (hdr, &ins, w, s ? s : "");
      free (s);
      if (e) {
        if (h > 0) e->text_height = h;
        /* attachment point (DXF 71): which of the 9 anchor positions the ins_pt
           denotes. dwg_add_MTEXT defaults to 1 (top-left); honor the real one. */
        if (att >= 1 && att <= 9) e->attachment = (BITCODE_BS) att;
        /* MTEXT rotation is the x-axis direction vector (DXF 11), not a code 50
           angle — this is what LibreDWG's DXF reader rejected. Encoding it
           directly here sidesteps that entirely. */
        e->x_axis_dir.x = cos (DEG2RAD (rot));
        e->x_axis_dir.y = sin (DEG2RAD (rot));
        e->x_axis_dir.z = 0;
      }
      set_common (dwg, e, lref, col);
    }
    else if (!strcmp (t, "DIM")) {
      /* DIM <li> <color> x1 y1 x2 y2 tx ty meas attach <text:lenstr>
         A real aligned DIMENSION: (x1,y1)/(x2,y2) are the two extension-line
         origins, (tx,ty) is the text + dimension-line point, meas is the
         measured length (DXF 42), attach is the text attachment (DXF 71, 1..9),
         and text is the override label shown verbatim (DXF 1).
         dwg_add_DIMENSION_ALIGNED ensures a Standard DIMSTYLE and links its
         handle (via DIMENSION_DEFAULTS), so there is no dangling dimstyle ref.
         No anonymous *D graphics block is baked — the CAD app regenerates the
         dimension drawing from the definition + dimstyle. */
      dwg_point_3d x1, x2, tm;
      x1.x = tok_dbl (&c); x1.y = tok_dbl (&c); x1.z = 0;
      x2.x = tok_dbl (&c); x2.y = tok_dbl (&c); x2.z = 0;
      tm.x = tok_dbl (&c); tm.y = tok_dbl (&c); tm.z = 0;
      double meas = tok_dbl (&c);
      int att = (int) tok_int (&c);
      char *s = tok_lenstr (&c);
      Dwg_Entity_DIMENSION_ALIGNED *e = dwg_add_DIMENSION_ALIGNED (hdr, &x1, &x2, &tm);
      if (e) {
        /* dimension-line definition point (DXF 10): put it on the text point so
           the dimension line runs through the label (matches the DXF writer,
           which sets 10 = 11 = text position). */
        e->def_pt.x = tm.x; e->def_pt.y = tm.y; e->def_pt.z = 0;
        e->act_measurement = meas;
        if (att >= 1 && att <= 9) e->attachment = (BITCODE_BS) att;
        /* override text (DXF 1) shown verbatim; dwg_free releases the T string. */
        if (s && *s) e->user_text = strdup (s);
      }
      free (s);
      set_common (dwg, e, lref, col);
    }
    else {
      /* unknown record type — bail rather than desync the stream */
      c.err = 1;
      break;
    }
  }

  int parse_err = c.err;
  free (layer_refs);
  free (buf);
  if (parse_err) { libredwg_skip_ref_dedup = 0; dwg_free (dwg); return 20; }

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
