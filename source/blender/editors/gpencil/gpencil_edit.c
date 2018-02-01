/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2008, Blender Foundation, Joshua Leung
 * This is a new part of Blender
 *
 * Contributor(s): Joshua Leung, Antonio Vazquez
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Operators for editing Grease Pencil strokes
 */

/** \file blender/editors/gpencil/gpencil_edit.c
 *  \ingroup edgpencil
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_workspace_types.h"

#include "BKE_main.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_paint.h"
#include "BKE_library.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_screen.h"
#include "BKE_workspace.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "UI_view2d.h"

#include "ED_gpencil.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_view3d.h"
#include "ED_space_api.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "gpencil_intern.h"

/* ************************************************ */
/* verify if is using the right brush */
static void gpencil_verify_brush_type(bContext *C, int newmode)
{
	ToolSettings *ts = CTX_data_tool_settings(C);
	GP_BrushEdit_Settings *gset = &ts->gp_sculpt;

	switch (newmode) {
		case OB_MODE_GPENCIL_SCULPT:
			gset->flag &= ~GP_BRUSHEDIT_FLAG_WEIGHT_MODE;
			if ((gset->brushtype < 0) || (gset->brushtype >= GP_EDITBRUSH_TYPE_WEIGHT)) {
				gset->brushtype = GP_EDITBRUSH_TYPE_PUSH;
			}
			break;
		case OB_MODE_GPENCIL_WEIGHT:
			gset->flag |= GP_BRUSHEDIT_FLAG_WEIGHT_MODE;
			if ((gset->weighttype < GP_EDITBRUSH_TYPE_WEIGHT) || (gset->weighttype >= TOT_GP_EDITBRUSH_TYPES)) {
				gset->weighttype = GP_EDITBRUSH_TYPE_WEIGHT;
			}
			break;
		default:
			break;
	}
}


/* setup modes and cursor */
static void gpencil_setup_modes(bContext *C, bGPdata *gpd, int newmode)
{
	if (!gpd) {
		return;
	}

	switch (newmode) {
		case OB_MODE_GPENCIL_EDIT:
			gpd->flag |= GP_DATA_STROKE_EDITMODE;
			gpd->flag &= ~GP_DATA_STROKE_PAINTMODE;
			gpd->flag &= ~GP_DATA_STROKE_SCULPTMODE;
			gpd->flag &= ~GP_DATA_STROKE_WEIGHTMODE;
			ED_gpencil_toggle_brush_cursor(C, false, NULL);
			break;
		case OB_MODE_GPENCIL_PAINT:
			gpd->flag &= ~GP_DATA_STROKE_EDITMODE;
			gpd->flag |= GP_DATA_STROKE_PAINTMODE;
			gpd->flag &= ~GP_DATA_STROKE_SCULPTMODE;
			gpd->flag &= ~GP_DATA_STROKE_WEIGHTMODE;
			ED_gpencil_toggle_brush_cursor(C, true, NULL);
			break;
		case OB_MODE_GPENCIL_SCULPT:
			gpd->flag &= ~GP_DATA_STROKE_EDITMODE;
			gpd->flag &= ~GP_DATA_STROKE_PAINTMODE;
			gpd->flag |= GP_DATA_STROKE_SCULPTMODE;
			gpd->flag &= ~GP_DATA_STROKE_WEIGHTMODE;
			gpencil_verify_brush_type(C, OB_MODE_GPENCIL_SCULPT);
			ED_gpencil_toggle_brush_cursor(C, true, NULL);
			break;
		case OB_MODE_GPENCIL_WEIGHT:
			gpd->flag &= ~GP_DATA_STROKE_EDITMODE;
			gpd->flag &= ~GP_DATA_STROKE_PAINTMODE;
			gpd->flag &= ~GP_DATA_STROKE_SCULPTMODE;
			gpd->flag |= GP_DATA_STROKE_WEIGHTMODE;
			gpencil_verify_brush_type(C, OB_MODE_GPENCIL_WEIGHT);
			ED_gpencil_toggle_brush_cursor(C, true, NULL);
			break;
		default:
			gpd->flag &= ~GP_DATA_STROKE_EDITMODE;
			gpd->flag &= ~GP_DATA_STROKE_PAINTMODE;
			gpd->flag &= ~GP_DATA_STROKE_SCULPTMODE;
			ED_gpencil_toggle_brush_cursor(C, false, NULL);
			break;
	}
}

/* Stroke Edit Mode Management */
static int gpencil_editmode_toggle_poll(bContext *C)
{
	/* if using gpencil object, use this gpd */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		return ob->data != NULL;
	}
	return ED_gpencil_data_get_active(C) != NULL;
}

static int gpencil_editmode_toggle_exec(bContext *C, wmOperator *op)
{
	const int back = RNA_int_get(op->ptr, "back");

	WorkSpace *workspace = CTX_wm_workspace(C);
	Scene *scene = CTX_data_scene(C);
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bool is_object = false;
	int mode;
	/* if using a gpencil object, use this datablock */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		gpd = ob->data;
		is_object = true;
	}
	
	if (gpd == NULL)
		return OPERATOR_CANCELLED;
	
	/* Just toggle editmode flag... */
	gpd->flag ^= GP_DATA_STROKE_EDITMODE;
	/* recalculate parent matrix */
	if (gpd->flag & GP_DATA_STROKE_EDITMODE) {
		ED_gpencil_reset_layers_parent(ob, gpd);
	}
	/* set mode */
	if (gpd->flag & GP_DATA_STROKE_EDITMODE) {
		mode = OB_MODE_GPENCIL_EDIT;
	}
	else {
		mode = OB_MODE_OBJECT;
	}

	if (is_object) {
		/* try to back previous mode */
		if ((ob->restore_mode) && ((gpd->flag & GP_DATA_STROKE_EDITMODE) == 0) && (back == 1)) {
			mode = ob->restore_mode;
		}
		ob->restore_mode = ob->mode;
		ob->mode = mode;
	}

	/* set workspace mode */
	BKE_workspace_object_mode_set(workspace, scene, mode);
	/* setup other modes */
	gpencil_setup_modes(C, gpd, mode);
	/* set cache as dirty */
	BKE_gpencil_batch_cache_dirty(gpd);

	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA, NULL);
	WM_event_add_notifier(C, NC_GPENCIL | ND_GPENCIL_EDITMODE, NULL);
	WM_event_add_notifier(C, NC_SCENE | ND_MODE, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_editmode_toggle(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Strokes Edit Mode Toggle";
	ot->idname = "GPENCIL_OT_editmode_toggle";
	ot->description = "Enter/Exit edit mode for Grease Pencil strokes";
	
	/* callbacks */
	ot->exec = gpencil_editmode_toggle_exec;
	ot->poll = gpencil_editmode_toggle_poll;
	
	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
	/* properties */
	RNA_def_int(ot->srna, "back", 0, 0, 1, "back", "1 to back previous mode", 0, 1);
}

/* Stroke Paint Mode Management */

static int gpencil_paintmode_toggle_poll(bContext *C)
{
	/* if using gpencil object, use this gpd */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		return ob->data != NULL;
	}
	return ED_gpencil_data_get_active(C) != NULL;
}

static int gpencil_paintmode_toggle_exec(bContext *C, wmOperator *op)
{
	const int back = RNA_int_get(op->ptr, "back");

	WorkSpace *workspace = CTX_wm_workspace(C);
	Scene *scene = CTX_data_scene(C);
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bool is_object = false;
	int mode;
	/* if using a gpencil object, use this datablock */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		gpd = ob->data;
		is_object = true;
	}

	if (gpd == NULL)
		return OPERATOR_CANCELLED;

	/* Just toggle paintmode flag... */
	gpd->flag ^= GP_DATA_STROKE_PAINTMODE;
	/* set mode */
	if (gpd->flag & GP_DATA_STROKE_PAINTMODE) {
		mode = OB_MODE_GPENCIL_PAINT;
	}
	else {
		mode = OB_MODE_OBJECT;
	}

	if (is_object) {
		/* try to back previous mode */
		if ((ob->restore_mode) && ((gpd->flag & GP_DATA_STROKE_PAINTMODE) == 0) && (back == 1)) {
			mode = ob->restore_mode;
		}
		ob->restore_mode = ob->mode;
		ob->mode = mode;
	}

	/* set workspace mode */
	BKE_workspace_object_mode_set(workspace, scene, mode);
	/* setup other modes */
	gpencil_setup_modes(C, gpd, mode);
	/* set cache as dirty */
	BKE_gpencil_batch_cache_dirty(gpd);

	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | ND_GPENCIL_EDITMODE, NULL);
	WM_event_add_notifier(C, NC_SCENE | ND_MODE, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_paintmode_toggle(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Strokes Paint Mode Toggle";
	ot->idname = "GPENCIL_OT_paintmode_toggle";
	ot->description = "Enter/Exit paint mode for Grease Pencil strokes";

	/* callbacks */
	ot->exec = gpencil_paintmode_toggle_exec;
	ot->poll = gpencil_paintmode_toggle_poll;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
	/* properties */
	RNA_def_int(ot->srna, "back", 0, 0, 1, "back", "1 to back previous mode", 0, 1);
}

/* Stroke Sculpt Mode Management */

static int gpencil_sculptmode_toggle_poll(bContext *C)
{
	/* if using gpencil object, use this gpd */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		return ob->data != NULL;
	}
	return ED_gpencil_data_get_active(C) != NULL;
}

static int gpencil_sculptmode_toggle_exec(bContext *C, wmOperator *op)
{
	const int back = RNA_int_get(op->ptr, "back");

	WorkSpace *workspace = CTX_wm_workspace(C);
	Scene *scene = CTX_data_scene(C);
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bool is_object = false;
	int mode;
	/* if using a gpencil object, use this datablock */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		gpd = ob->data;
		is_object = true;
	}

	if (gpd == NULL)
		return OPERATOR_CANCELLED;

	/* Just toggle sculptmode flag... */
	gpd->flag ^= GP_DATA_STROKE_SCULPTMODE;
	/* set mode */
	if (gpd->flag & GP_DATA_STROKE_SCULPTMODE) {
		mode = OB_MODE_GPENCIL_SCULPT;
	}
	else {
		mode = OB_MODE_OBJECT;
	}

	if (is_object) {
		/* try to back previous mode */
		if ((ob->restore_mode) && ((gpd->flag & GP_DATA_STROKE_SCULPTMODE) == 0) && (back == 1)) {
			mode = ob->restore_mode;
		}
		ob->restore_mode = ob->mode;
		ob->mode = mode;
	}

	/* set workspace mode */
	BKE_workspace_object_mode_set(workspace, scene, mode);
	/* setup other modes */
	gpencil_setup_modes(C, gpd, mode);
	/* set cache as dirty */
	BKE_gpencil_batch_cache_dirty(gpd);

	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | ND_GPENCIL_EDITMODE, NULL);
	WM_event_add_notifier(C, NC_SCENE | ND_MODE, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_sculptmode_toggle(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Strokes Sculpt Mode Toggle";
	ot->idname = "GPENCIL_OT_sculptmode_toggle";
	ot->description = "Enter/Exit sculpt mode for Grease Pencil strokes";

	/* callbacks */
	ot->exec = gpencil_sculptmode_toggle_exec;
	ot->poll = gpencil_sculptmode_toggle_poll;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
	/* properties */
	RNA_def_int(ot->srna, "back", 0, 0, 1, "back", "1 to back previous mode", 0, 1);
}

/* Stroke Weight Paint Mode Management */

static int gpencil_weightmode_toggle_poll(bContext *C)
{
	/* if using gpencil object, use this gpd */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		return ob->data != NULL;
	}
	return ED_gpencil_data_get_active(C) != NULL;
}

static int gpencil_weightmode_toggle_exec(bContext *C, wmOperator *op)
{
	const int back = RNA_int_get(op->ptr, "back");

	WorkSpace *workspace = CTX_wm_workspace(C);
	Scene *scene = CTX_data_scene(C);
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bool is_object = false;
	int mode;
	/* if using a gpencil object, use this datablock */
	Object *ob = CTX_data_active_object(C);
	if ((ob) && (ob->type == OB_GPENCIL)) {
		gpd = ob->data;
		is_object = true;
	}

	if (gpd == NULL)
		return OPERATOR_CANCELLED;

	/* Just toggle weightmode flag... */
	gpd->flag ^= GP_DATA_STROKE_WEIGHTMODE;
	/* set mode */
	if (gpd->flag & GP_DATA_STROKE_WEIGHTMODE) {
		mode = OB_MODE_GPENCIL_WEIGHT;
	}
	else {
		mode = OB_MODE_OBJECT;
	}

	if (is_object) {
		/* try to back previous mode */
		if ((ob->restore_mode) && ((gpd->flag & GP_DATA_STROKE_WEIGHTMODE) == 0) && (back == 1)) {
			mode = ob->restore_mode;
		}
		ob->restore_mode = ob->mode;
		ob->mode = mode;
	}

	/* set workspace mode */
	BKE_workspace_object_mode_set(workspace, scene, mode);
	/* setup other modes */
	gpencil_setup_modes(C, gpd, mode);
	/* set cache as dirty */
	BKE_gpencil_batch_cache_dirty(gpd);

	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | ND_GPENCIL_EDITMODE, NULL);
	WM_event_add_notifier(C, NC_SCENE | ND_MODE, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_weightmode_toggle(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Strokes Weight Mode Toggle";
	ot->idname = "GPENCIL_OT_weightmode_toggle";
	ot->description = "Enter/Exit weight paint mode for Grease Pencil strokes";

	/* callbacks */
	ot->exec = gpencil_weightmode_toggle_exec;
	ot->poll = gpencil_weightmode_toggle_poll;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
	/* properties */
	RNA_def_int(ot->srna, "back", 0, 0, 1, "back", "1 to back previous mode", 0, 1);
}

/* ************************************************ */
/* Stroke Editing Operators */

/* poll callback for all stroke editing operators */
static int gp_stroke_edit_poll(bContext *C)
{
	/* NOTE: this is a bit slower, but is the most accurate... */
	return CTX_DATA_COUNT(C, editable_gpencil_strokes) != 0;
}

/* poll callback to verify edit mode in 3D view only */
static int gp_strokes_edit3d_poll(bContext *C)
{
	/* 2 Requirements:
	*  - 1) Editable GP data
	*  - 2) 3D View only
	*/
	return (gp_stroke_edit_poll(C) && ED_operator_view3d_active(C));
}

/* ************ Stroke Hide selection Toggle ************** */

static int gpencil_hideselect_toggle_exec(bContext *C, wmOperator *UNUSED(op))
{
	ToolSettings *ts = CTX_data_tool_settings(C);

	if (ts == NULL)
		return OPERATOR_CANCELLED;

	/* Just toggle alpha... */
	if (ts->gp_sculpt.alpha > 0.0f) {
		ts->gp_sculpt.alpha = 0.0f;
	}
	else {
		ts->gp_sculpt.alpha = 1.0f;
	}

	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA, NULL);
	WM_event_add_notifier(C, NC_GPENCIL | ND_GPENCIL_EDITMODE, NULL);
	WM_event_add_notifier(C, NC_SCENE | ND_MODE, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_selection_opacity_toggle(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Hide Selection";
	ot->idname = "GPENCIL_OT_selection_opacity_toggle";
	ot->description = "Hide/Unhide selected points for Grease Pencil strokes setting alpha factor";

	/* callbacks */
	ot->exec = gpencil_hideselect_toggle_exec;
	ot->poll = gp_stroke_edit_poll;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
}

/* toggle multi edit strokes support */
static int gpencil_multiedit_toggle_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	const int lines = RNA_int_get(op->ptr, "lines");

	if (gpd == NULL)
		return OPERATOR_CANCELLED;

	/* Just toggle value */
	if (lines == 0) {
		gpd->flag ^= GP_DATA_STROKE_SHOW_EDIT_LINES;
	}
	else {
		gpd->flag ^= GP_DATA_STROKE_MULTIEDIT_LINES;
	}

	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | ND_GPENCIL_EDITMODE, NULL);
	WM_event_add_notifier(C, NC_SCENE | ND_MODE, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_multiedit_toggle(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Edit Lines Toggle";
	ot->idname = "GPENCIL_OT_multiedit_toggle";
	ot->description = "Enable/Disable edit lines support";

	/* callbacks */
	ot->exec = gpencil_multiedit_toggle_exec;
	ot->poll = gp_stroke_edit_poll;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;

	/* properties */
	RNA_def_int(ot->srna, "lines", 0, 0, 1, "lines", "1 to toggle display lines only", 0, 1);
}

/* ************** Duplicate Selected Strokes **************** */

/* Make copies of selected point segments in a selected stroke */
static void gp_duplicate_points(const bGPDstroke *gps, ListBase *new_strokes, const char *layername)
{
	bGPDspoint *pt;
	int i;
	
	int start_idx = -1;
	
	
	/* Step through the original stroke's points:
	 * - We accumulate selected points (from start_idx to current index)
	 *   and then convert that to a new stroke
	 */
	for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
		/* searching for start, are waiting for end? */
		if (start_idx == -1) {
			/* is this the first selected point for a new island? */
			if (pt->flag & GP_SPOINT_SELECT) {
				start_idx = i;
			}
		}
		else {
			size_t len = 0;
			
			/* is this the end of current island yet?
			 * 1) Point i-1 was the last one that was selected
			 * 2) Point i is the last in the array
			 */
			if ((pt->flag & GP_SPOINT_SELECT) == 0) {
				len = i - start_idx;
			}
			else if (i == gps->totpoints - 1) {
				len = i - start_idx + 1;
			}
			//printf("copying from %d to %d = %d\n", start_idx, i, len);
		
			/* make copies of the relevant data */
			if (len) {
				bGPDstroke *gpsd;
				
				/* make a stupid copy first of the entire stroke (to get the flags too) */
				gpsd = MEM_dupallocN(gps);
				BLI_strncpy(gpsd->tmp_layerinfo, layername, sizeof(gpsd->tmp_layerinfo)); /* saves original layer name */
				
				/* initialize triangle memory - will be calculated on next redraw */
				gpsd->triangles = NULL;
				gpsd->flag |= GP_STROKE_RECALC_CACHES;
				gpsd->tot_triangles = 0;
				
				/* now, make a new points array, and copy of the relevant parts */
				gpsd->points = MEM_callocN(sizeof(bGPDspoint) * len, "gps stroke points copy");
				memcpy(gpsd->points, gps->points + start_idx, sizeof(bGPDspoint) * len);
				gpsd->totpoints = len;
				/* Copy weights */
				int e = start_idx;
				for (int j = 0; j < gpsd->totpoints; ++j) {
					bGPDspoint *pt_src = &gps->points[e];
					bGPDspoint *pt_dst = &gpsd->points[j];
					pt_dst->weights = MEM_dupallocN(pt_src->weights);
					++e;
				}

				/* add to temp buffer */
				gpsd->next = gpsd->prev = NULL;
				BLI_addtail(new_strokes, gpsd);
				
				/* cleanup + reset for next */
				start_idx = -1;
			}
		}
	}
}

static int gp_duplicate_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);

	if (gpd == NULL) {
		BKE_report(op->reports, RPT_ERROR, "No Grease Pencil data");
		return OPERATOR_CANCELLED;
	}

	if (GPENCIL_MULTIEDIT_SESSIONS_ON(gpd)) {
		BKE_report(op->reports, RPT_ERROR, "Operator not supported in multiframe edition");
		return OPERATOR_CANCELLED;
	}
	
	/* for each visible (and editable) layer's selected strokes,
	 * copy the strokes into a temporary buffer, then append
	 * once all done
	 */
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		ListBase new_strokes = {NULL, NULL};
		bGPDframe *gpf = gpl->actframe;
		bGPDstroke *gps;
		
		if (gpf == NULL)
			continue;
		
		/* make copies of selected strokes, and deselect these once we're done */
		for (gps = gpf->strokes.first; gps; gps = gps->next) {
			/* skip strokes that are invalid for current view */
			if (ED_gpencil_stroke_can_use(C, gps) == false) {
				continue;
			}
			
			if (gps->flag & GP_STROKE_SELECT) {
				if (gps->totpoints == 1) {
					/* Special Case: If there's just a single point in this stroke... */
					bGPDstroke *gpsd;
					
					/* make direct copies of the stroke and its points */
					gpsd = MEM_dupallocN(gps);
					BLI_strncpy(gpsd->tmp_layerinfo, gpl->info, sizeof(gpsd->tmp_layerinfo));
					gpsd->points = MEM_dupallocN(gps->points);
					BKE_gpencil_stroke_weights_duplicate(gps, gpsd);

					/* triangle information - will be calculated on next redraw */
					gpsd->flag |= GP_STROKE_RECALC_CACHES;
					gpsd->triangles = NULL;
					
					/* add to temp buffer */
					gpsd->next = gpsd->prev = NULL;
					BLI_addtail(&new_strokes, gpsd);
				}
				else {
					/* delegate to a helper, as there's too much to fit in here (for copying subsets)... */
					gp_duplicate_points(gps, &new_strokes, gpl->info);
				}
				
				/* deselect original stroke, or else the originals get moved too
				 * (when using the copy + move macro)
				 */
				gps->flag &= ~GP_STROKE_SELECT;
			}
		}
		
		/* add all new strokes in temp buffer to the frame (preventing double-copies) */
		BLI_movelisttolist(&gpf->strokes, &new_strokes);
		BLI_assert(new_strokes.first == NULL);
	}
	CTX_DATA_END;
	
	/* updates */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_duplicate(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Duplicate Strokes";
	ot->idname = "GPENCIL_OT_duplicate";
	ot->description = "Duplicate the selected Grease Pencil strokes";
	
	/* callbacks */
	ot->exec = gp_duplicate_exec;
	ot->poll = gp_stroke_edit_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ******************* Copy/Paste Strokes ************************* */
/* Grease Pencil stroke data copy/paste buffer:
 * - The copy operation collects all segments of selected strokes,
 *   dumping "ready to be copied" copies of the strokes into the buffer.
 * - The paste operation makes a copy of those elements, and adds them
 *   to the active layer. This effectively flattens down the strokes
 *   from several different layers into a single layer.
 */

/* list of bGPDstroke instances */
/* NOTE: is exposed within the editors/gpencil module so that other tools can use it too */
ListBase gp_strokes_copypastebuf = {NULL, NULL};

/* Hash for hanging on to all the palette colors used by strokes in the buffer
 *
 * This is needed to prevent dangling and unsafe pointers when pasting across datablocks,
 * or after a color used by a stroke in the buffer gets deleted (via user action or undo).
 */
static GHash *gp_strokes_copypastebuf_colors = NULL;

/* Free copy/paste buffer data */
void ED_gpencil_strokes_copybuf_free(void)
{
	bGPDstroke *gps, *gpsn;
	
	/* Free the palettes buffer
	 * NOTE: This is done before the strokes so that the name ptrs (keys) are still safe
	 */
	if (gp_strokes_copypastebuf_colors) {
		BLI_ghash_free(gp_strokes_copypastebuf_colors, NULL, MEM_freeN);
		gp_strokes_copypastebuf_colors = NULL;
	}
	
	/* Free the stroke buffer */
	for (gps = gp_strokes_copypastebuf.first; gps; gps = gpsn) {
		gpsn = gps->next;
		
		if (gps->points) {
			BKE_gpencil_free_stroke_weights(gps);
			MEM_freeN(gps->points);
		}
		if (gps->triangles) MEM_freeN(gps->triangles);

		BLI_freelinkN(&gp_strokes_copypastebuf, gps);
	}
	
	gp_strokes_copypastebuf.first = gp_strokes_copypastebuf.last = NULL;
}

/* Ensure that destination datablock has all the colours the pasted strokes need
 * Helper function for copy-pasting strokes
 */
GHash *gp_copybuf_validate_colormap(bContext *C)
{
	GHash *new_colors = BLI_ghash_str_new("GPencil Paste Dst Colors");
	GHashIterator gh_iter;
	
	bGPdata *gpd = CTX_data_gpencil_data(C);
	bGPDpaletteref *palslot;
	Palette *palette;
	
	/* If there's no active palette yet (i.e. new datablock), add one */
	palslot = BKE_gpencil_paletteslot_validate(CTX_data_main(C), gpd);
	palette = palslot->palette;
	
	/* For each color, figure out what to map to... */
	GHASH_ITER(gh_iter, gp_strokes_copypastebuf_colors) {
		PaletteColor *palcolor;
		char *name = BLI_ghashIterator_getKey(&gh_iter);
		
		/* Look for existing color to map to */
		/* XXX: What to do if same name but different color? Behaviour here should depend on a property? */
		palcolor = BKE_palette_color_getbyname(palette, name);
		if (palcolor == NULL) {
			/* Doesn't Exist - Create new matching color for this palette */
			/* XXX: This still doesn't fix the pasting across file boundaries problem... */
			PaletteColor *src_color = BLI_ghashIterator_getValue(&gh_iter);
			
			palcolor = MEM_dupallocN(src_color);
			BLI_addtail(&palette->colors, palcolor);
			
			BLI_uniquename(&palette->colors, palcolor, DATA_("GP Color"), '.', offsetof(PaletteColor, info), sizeof(palcolor->info));
		}
		
		/* Store this mapping (for use later when pasting) */
		BLI_ghash_insert(new_colors, name, palcolor);
	}
	
	return new_colors;
}

/* --------------------- */
/* Copy selected strokes */

static int gp_strokes_copy_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	
	if (gpd == NULL) {
		BKE_report(op->reports, RPT_ERROR, "No Grease Pencil data");
		return OPERATOR_CANCELLED;
	}

	if (GPENCIL_MULTIEDIT_SESSIONS_ON(gpd)) {
		BKE_report(op->reports, RPT_ERROR, "Operator not supported in multiframe edition");
		return OPERATOR_CANCELLED;
	}

	/* clear the buffer first */
	ED_gpencil_strokes_copybuf_free();
	
	/* for each visible (and editable) layer's selected strokes,
	 * copy the strokes into a temporary buffer, then append
	 * once all done
	 */
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *gpf = gpl->actframe;
		bGPDstroke *gps;
		
		if (gpf == NULL)
			continue;
		
		/* make copies of selected strokes, and deselect these once we're done */
		for (gps = gpf->strokes.first; gps; gps = gps->next) {
			/* skip strokes that are invalid for current view */
			if (ED_gpencil_stroke_can_use(C, gps) == false)
				continue;
			
			if (gps->flag & GP_STROKE_SELECT) {
				if (gps->totpoints == 1) {
					/* Special Case: If there's just a single point in this stroke... */
					bGPDstroke *gpsd;
					
					/* make direct copies of the stroke and its points */
					gpsd = MEM_dupallocN(gps);
					BLI_strncpy(gpsd->tmp_layerinfo, gpl->info, sizeof(gpsd->tmp_layerinfo)); /* saves original layer name */
					gpsd->points = MEM_dupallocN(gps->points);
					BKE_gpencil_stroke_weights_duplicate(gps, gpsd);

					/* triangles cache - will be recalculated on next redraw */
					gpsd->flag |= GP_STROKE_RECALC_CACHES;
					gpsd->tot_triangles = 0;
					gpsd->triangles = NULL;
					
					/* add to temp buffer */
					gpsd->next = gpsd->prev = NULL;
					BLI_addtail(&gp_strokes_copypastebuf, gpsd);
				}
				else {
					/* delegate to a helper, as there's too much to fit in here (for copying subsets)... */
					gp_duplicate_points(gps, &gp_strokes_copypastebuf, gpl->info);
				}
			}
		}
	}
	CTX_DATA_END;
	
	/* Build up hash of colors used in these strokes, making copies of these to protect against dangling pointers */
	if (gp_strokes_copypastebuf.first) {
		gp_strokes_copypastebuf_colors = BLI_ghash_str_new("GPencil CopyBuf Colors");
		
		for (bGPDstroke *gps = gp_strokes_copypastebuf.first; gps; gps = gps->next) {
			if (ED_gpencil_stroke_can_use(C, gps)) {
				if (BLI_ghash_haskey(gp_strokes_copypastebuf_colors, gps->colorname) == false) {
					PaletteColor *color = MEM_dupallocN(gps->palcolor);
					
					BLI_ghash_insert(gp_strokes_copypastebuf_colors, gps->colorname, color);
					gps->palcolor = color;
				}
			}
		}
	}
	
	/* updates (to ensure operator buttons are refreshed, when used via hotkeys) */
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA, NULL); // XXX?
	
	/* done */
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_copy(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Copy Strokes";
	ot->idname = "GPENCIL_OT_copy";
	ot->description = "Copy selected Grease Pencil points and strokes";
	
	/* callbacks */
	ot->exec = gp_strokes_copy_exec;
	ot->poll = gp_stroke_edit_poll;
	
	/* flags */
	//ot->flag = OPTYPE_REGISTER;
}

/* --------------------- */
/* Paste selected strokes */

static int gp_strokes_paste_poll(bContext *C)
{
	/* 1) Must have GP datablock to paste to
	 *    - We don't need to have an active layer though, as that can easily get added
	 *    - If the active layer is locked, we can't paste there, but that should prompt a warning instead
	 * 2) Copy buffer must at least have something (though it may be the wrong sort...)
	 */
	return (ED_gpencil_data_get_active(C) != NULL) && (!BLI_listbase_is_empty(&gp_strokes_copypastebuf));
}

typedef enum eGP_PasteMode {
	GP_COPY_ONLY = -1,
	GP_COPY_MERGE = 1
} eGP_PasteMode;

static int gp_strokes_paste_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDlayer *gpl = CTX_data_active_gpencil_layer(C); /* only use active for copy merge */
	bGPDframe *gpf;
	
	eGP_PasteMode type = RNA_enum_get(op->ptr, "type");
	GHash *new_colors;
	
	/* check for various error conditions */
	if (gpd == NULL) {
		BKE_report(op->reports, RPT_ERROR, "No Grease Pencil data");
		return OPERATOR_CANCELLED;
	}
	else if (GPENCIL_MULTIEDIT_SESSIONS_ON(gpd)) {
		BKE_report(op->reports, RPT_ERROR, "Operator not supported in multiframe edition");
		return OPERATOR_CANCELLED;
	}
	else if (BLI_listbase_is_empty(&gp_strokes_copypastebuf)) {
		BKE_report(op->reports, RPT_ERROR, "No strokes to paste, select and copy some points before trying again");
		return OPERATOR_CANCELLED;
	}
	else if (gpl == NULL) {
		/* no active layer - let's just create one */
		gpl = BKE_gpencil_layer_addnew(gpd, DATA_("GP_Layer"), true);
	}
	else if ((gpencil_layer_is_editable(gpl) == false) && (type == GP_COPY_MERGE)) {
		BKE_report(op->reports, RPT_ERROR, "Can not paste strokes when active layer is hidden or locked");
		return OPERATOR_CANCELLED;
	}
	else {
		/* Check that some of the strokes in the buffer can be used */
		bGPDstroke *gps;
		bool ok = false;
		
		for (gps = gp_strokes_copypastebuf.first; gps; gps = gps->next) {
			if (ED_gpencil_stroke_can_use(C, gps)) {
				ok = true;
				break;
			}
		}
		
		if (ok == false) {
			/* XXX: this check is not 100% accurate (i.e. image editor is incompatible with normal 2D strokes),
			 * but should be enough to give users a good idea of what's going on
			 */
			if (CTX_wm_area(C)->spacetype == SPACE_VIEW3D)
				BKE_report(op->reports, RPT_ERROR, "Cannot paste 2D strokes in 3D View");
			else
				BKE_report(op->reports, RPT_ERROR, "Cannot paste 3D strokes in 2D editors");
				
			return OPERATOR_CANCELLED;
		}
	}
	
	/* Deselect all strokes first */
	CTX_DATA_BEGIN(C, bGPDstroke *, gps, editable_gpencil_strokes)
	{
		bGPDspoint *pt;
		int i;
		
		for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
			pt->flag &= ~GP_SPOINT_SELECT;
		}
		
		gps->flag &= ~GP_STROKE_SELECT;
	}
	CTX_DATA_END;
	
	/* Ensure that all the necessary colors exist */
	new_colors = gp_copybuf_validate_colormap(C);
		
	/* Copy over the strokes from the buffer (and adjust the colors) */
	for (bGPDstroke *gps = gp_strokes_copypastebuf.first; gps; gps = gps->next) {
		if (ED_gpencil_stroke_can_use(C, gps)) {
			/* Need to verify if layer exists */
			if (type != GP_COPY_MERGE) {
				gpl = BLI_findstring(&gpd->layers, gps->tmp_layerinfo, offsetof(bGPDlayer, info));
				if (gpl == NULL) {
					/* no layer - use active (only if layer deleted before paste) */
					gpl = CTX_data_active_gpencil_layer(C);
				}
			}
			
			/* Ensure we have a frame to draw into
			 * NOTE: Since this is an op which creates strokes,
			 *       we are obliged to add a new frame if one
			 *       doesn't exist already
			 */
			gpf = BKE_gpencil_layer_getframe(gpl, CFRA, true);
			if (gpf) {
				/* Create new stroke */
				bGPDstroke *new_stroke = MEM_dupallocN(gps);
				new_stroke->tmp_layerinfo[0] = '\0';
				
				new_stroke->points = MEM_dupallocN(gps->points);
				BKE_gpencil_stroke_weights_duplicate(gps, new_stroke);

				new_stroke->flag |= GP_STROKE_RECALC_CACHES;
				new_stroke->triangles = NULL;
				
				new_stroke->next = new_stroke->prev = NULL;
				BLI_addtail(&gpf->strokes, new_stroke);
				
				/* Fix color references */
				BLI_assert(new_stroke->colorname[0] != '\0');
				new_stroke->palcolor = BLI_ghash_lookup(new_colors, new_stroke->colorname);
				
				BLI_assert(new_stroke->palcolor != NULL);
				BLI_strncpy(new_stroke->colorname, new_stroke->palcolor->info, sizeof(new_stroke->colorname));
				
				/*new_stroke->flag |= GP_STROKE_RECALC_COLOR; */
			}
		}
	}
	
	/* free temp data */
	BLI_ghash_free(new_colors, NULL, NULL);
	
	/* updates */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_paste(wmOperatorType *ot)
{
	static const EnumPropertyItem copy_type[] = {
		{GP_COPY_ONLY, "COPY", 0, "Copy", ""},
		{GP_COPY_MERGE, "MERGE", 0, "Merge", ""},
		{0, NULL, 0, NULL, NULL}
	};
	
	/* identifiers */
	ot->name = "Paste Strokes";
	ot->idname = "GPENCIL_OT_paste";
	ot->description = "Paste previously copied strokes or copy and merge in active layer";
	
	/* callbacks */
	ot->exec = gp_strokes_paste_exec;
	ot->poll = gp_strokes_paste_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* properties */
	ot->prop = RNA_def_enum(ot->srna, "type", copy_type, 0, "Type", "");
}

/* ******************* Move To Layer ****************************** */

static int gp_move_to_layer_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(evt))
{
	uiPopupMenu *pup;
	uiLayout *layout;
	
	/* call the menu, which will call this operator again, hence the canceled */
	pup = UI_popup_menu_begin(C, op->type->name, ICON_NONE);
	layout = UI_popup_menu_layout(pup);
	uiItemsEnumO(layout, "GPENCIL_OT_move_to_layer", "layer");
	UI_popup_menu_end(C, pup);
	
	return OPERATOR_INTERFACE;
}

// FIXME: allow moving partial strokes
static int gp_move_to_layer_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = CTX_data_gpencil_data(C);
	bGPDlayer *target_layer = NULL;
	ListBase strokes = {NULL, NULL};
	int layer_num = RNA_enum_get(op->ptr, "layer");

	if (GPENCIL_MULTIEDIT_SESSIONS_ON(gpd)) {
		BKE_report(op->reports, RPT_ERROR, "Operator not supported in multiframe edition");
		return OPERATOR_CANCELLED;
	}

	/* Get layer or create new one */
	if (layer_num == -1) {
		/* Create layer */
		target_layer = BKE_gpencil_layer_addnew(gpd, DATA_("GP_Layer"), true);
	}
	else {
		/* Try to get layer */
		target_layer = BLI_findlink(&gpd->layers, layer_num);
		
		if (target_layer == NULL) {
			BKE_reportf(op->reports, RPT_ERROR, "There is no layer number %d", layer_num);
			return OPERATOR_CANCELLED;
		}
	}
	
	/* Extract all strokes to move to this layer
	 * NOTE: We need to do this in a two-pass system to avoid conflicts with strokes
	 *       getting repeatedly moved
	 */
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *gpf = gpl->actframe;
		bGPDstroke *gps, *gpsn;
		
		/* skip if no frame with strokes, or if this is the layer we're moving strokes to */
		if ((gpl == target_layer) || (gpf == NULL))
			continue;
		
		/* make copies of selected strokes, and deselect these once we're done */
		for (gps = gpf->strokes.first; gps; gps = gpsn) {
			gpsn = gps->next;
			
			/* skip strokes that are invalid for current view */
			if (ED_gpencil_stroke_can_use(C, gps) == false)
				continue;
			
			/* TODO: Don't just move entire strokes - instead, only copy the selected portions... */
			if (gps->flag & GP_STROKE_SELECT) {
				BLI_remlink(&gpf->strokes, gps);
				BLI_addtail(&strokes, gps);
			}
		}
	}
	CTX_DATA_END;
	
	/* Paste them all in one go */
	if (strokes.first) {
		Scene *scene = CTX_data_scene(C);
		bGPDframe *gpf = BKE_gpencil_layer_getframe(target_layer, CFRA, true);
		
		BLI_movelisttolist(&gpf->strokes, &strokes);
		BLI_assert((strokes.first == strokes.last) && (strokes.first == NULL));
	}
	
	/* updates */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_move_to_layer(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Move Strokes to Layer";
	ot->idname = "GPENCIL_OT_move_to_layer";
	ot->description = "Move selected strokes to another layer"; // XXX: allow moving individual points too?
	
	/* callbacks */
	ot->invoke = gp_move_to_layer_invoke;
	ot->exec = gp_move_to_layer_exec;
	ot->poll = gp_stroke_edit_poll; // XXX?
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* gp layer to use (dynamic enum) */
	ot->prop = RNA_def_enum(ot->srna, "layer", DummyRNA_DEFAULT_items, 0, "Grease Pencil Layer", "");
	RNA_def_enum_funcs(ot->prop, ED_gpencil_layers_with_new_enum_itemf);
}

/* ********************* Add Blank Frame *************************** */

/* Basically the same as the drawing op */
static int UNUSED_FUNCTION(gp_blank_frame_add_poll)(bContext *C)
{
	if (ED_operator_regionactive(C)) {
		/* check if current context can support GPencil data */
		if (ED_gpencil_data_get_pointers(C, NULL) != NULL) {
			return 1;
		}
		else {
			CTX_wm_operator_poll_msg_set(C, "Failed to find Grease Pencil data to draw into");
		}
	}
	else {
		CTX_wm_operator_poll_msg_set(C, "Active region not set");
	}
	
	return 0;
}

static int gp_blank_frame_add_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDlayer *active_gpl = BKE_gpencil_layer_getactive(gpd);

	const bool all_layers = RNA_boolean_get(op->ptr, "all_layers");

	/* Initialise datablock and an active layer if nothing exists yet */
	if (ELEM(NULL, gpd, active_gpl)) {
		/* let's just be lazy, and call the "Add New Layer" operator, which sets everything up as required */
		WM_operator_name_call(C, "GPENCIL_OT_layer_add", WM_OP_EXEC_DEFAULT, NULL);
	}
	
	/* Go through each layer, adding a frame after the active one 
	 * and/or shunting all the others out of the way
	 */
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		if ((all_layers == false) && (gpl != active_gpl)) {
			continue;
		}

		/* 1) Check for an existing frame on the current frame */
		bGPDframe *gpf = BKE_gpencil_layer_find_frame(gpl, CFRA);
		if (gpf) {
			/* Shunt all frames after (and including) the existing one later by 1-frame */
			for (; gpf; gpf = gpf->next) {
				gpf->framenum += 1;
			}
		}
		
		/* 2) Now add a new frame, with nothing in it */
		gpl->actframe = BKE_gpencil_layer_getframe(gpl, CFRA, GP_GETFRAME_ADD_NEW);
	}
	CTX_DATA_END;
	
	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_blank_frame_add(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Insert Blank Frame";
	ot->idname = "GPENCIL_OT_blank_frame_add";
	ot->description = "Insert a blank frame on the current frame "
	                  "(all subsequently existing frames, if any, are shifted right by one frame)";
	
	/* callbacks */
	ot->exec = gp_blank_frame_add_exec;
	ot->poll = gp_add_poll;
	
	/* properties */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	RNA_def_boolean(ot->srna, "all_layers", false, "All Layers", "Create blank frame in all layers, not only active");
}

/* ******************* Delete Active Frame ************************ */

static int gp_actframe_delete_poll(bContext *C)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDlayer *gpl = BKE_gpencil_layer_getactive(gpd);
	
	/* only if there's an active layer with an active frame */
	return (gpl && gpl->actframe);
}

/* delete active frame - wrapper around API calls */
static int gp_actframe_delete_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDlayer *gpl = BKE_gpencil_layer_getactive(gpd);
	bGPDframe *gpf = BKE_gpencil_layer_getframe(gpl, CFRA, 0);
	
	/* if there's no existing Grease-Pencil data there, add some */
	if (gpd == NULL) {
		BKE_report(op->reports, RPT_ERROR, "No grease pencil data");
		return OPERATOR_CANCELLED;
	}
	if (ELEM(NULL, gpl, gpf)) {
		BKE_report(op->reports, RPT_ERROR, "No active frame to delete");
		return OPERATOR_CANCELLED;
	}
	
	/* delete it... */
	BKE_gpencil_layer_delframe(gpl, gpf);
	
	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_active_frame_delete(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Delete Active Frame";
	ot->idname = "GPENCIL_OT_active_frame_delete";
	ot->description = "Delete the active frame for the active Grease Pencil Layer";
	
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* callbacks */
	ot->exec = gp_actframe_delete_exec;
	ot->poll = gp_actframe_delete_poll;
}

/* **************** Delete All Active Frames ****************** */

static int gp_actframe_delete_all_poll(bContext *C)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	
	/* 1) There must be grease pencil data
	 * 2) Hopefully some of the layers have stuff we can use
	 */
	return (gpd && gpd->layers.first);
}

static int gp_actframe_delete_all_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	Scene *scene = CTX_data_scene(C);
	bool success = false;
	
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		/* try to get the "active" frame - but only if it actually occurs on this frame */
		bGPDframe *gpf = BKE_gpencil_layer_getframe(gpl, CFRA, 0);
		
		if (gpf == NULL)
			continue;
		
		/* delete it... */
		BKE_gpencil_layer_delframe(gpl, gpf);
		
		/* we successfully modified something */
		success = true;
	}
	CTX_DATA_END;
	
	/* updates */
	if (success) {
		BKE_gpencil_batch_cache_dirty(gpd);
		WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
		return OPERATOR_FINISHED;
	}
	else {
		BKE_report(op->reports, RPT_ERROR, "No active frame(s) to delete");
		return OPERATOR_CANCELLED;
	}
}

void GPENCIL_OT_active_frames_delete_all(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Delete All Active Frames";
	ot->idname = "GPENCIL_OT_active_frames_delete_all";
	ot->description = "Delete the active frame(s) of all editable Grease Pencil layers";
	
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* callbacks */
	ot->exec = gp_actframe_delete_all_exec;
	ot->poll = gp_actframe_delete_all_poll;
}

/* ******************* Delete Operator ************************ */

typedef enum eGP_DeleteMode {
	/* delete selected stroke points */
	GP_DELETEOP_POINTS          = 0,
	/* delete selected strokes */
	GP_DELETEOP_STROKES         = 1,
	/* delete active frame */
	GP_DELETEOP_FRAME           = 2,
} eGP_DeleteMode;

typedef enum eGP_DissolveMode {
	/* dissolve all selected points */
	GP_DISSOLVE_POINTS = 0,
	/* dissolve between selected points */
	GP_DISSOLVE_BETWEEN = 1,
	/* dissolve unselected points */
	GP_DISSOLVE_UNSELECT = 2,
} eGP_DissolveMode;

/* ----------------------------------- */

/* Delete selected strokes */
static int gp_delete_selected_strokes(bContext *C)
{
	bool changed = false;
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bool is_multiedit = (bool)GPENCIL_MULTIEDIT_SESSIONS_ON(gpd);

	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *init_gpf = gpl->actframe;
		if (is_multiedit) {
			init_gpf = gpl->frames.first;
		}

		for (bGPDframe *gpf = init_gpf; gpf; gpf = gpf->next) {
			if ((gpf == gpl->actframe) || ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {
				bGPDstroke *gps, *gpsn;

				if (gpf == NULL)
					continue;

				/* simply delete strokes which are selected */
				for (gps = gpf->strokes.first; gps; gps = gpsn) {
					gpsn = gps->next;

					/* skip strokes that are invalid for current view */
					if (ED_gpencil_stroke_can_use(C, gps) == false)
						continue;

					/* free stroke if selected */
					if (gps->flag & GP_STROKE_SELECT) {
						/* free stroke memory arrays, then stroke itself */
						if (gps->points) {
							BKE_gpencil_free_stroke_weights(gps);
							MEM_freeN(gps->points);
						}
						if (gps->triangles) MEM_freeN(gps->triangles);
						BLI_freelinkN(&gpf->strokes, gps);

						changed = true;
					}
				}
			}
		}
	}
	CTX_DATA_END;
	
	if (changed) {
		BKE_gpencil_batch_cache_dirty(gpd);
		WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
		return OPERATOR_FINISHED;
	}
	else {
		return OPERATOR_CANCELLED;
	}
}

/* ----------------------------------- */

/* Delete selected points but keep the stroke */
static int gp_dissolve_selected_points(bContext *C, eGP_DissolveMode mode)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bool is_multiedit = (bool)GPENCIL_MULTIEDIT_SESSIONS_ON(gpd);
	bool changed = false;
	int first, last;

	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *init_gpf = gpl->actframe;
		if (is_multiedit) {
			init_gpf = gpl->frames.first;
		}

		for (bGPDframe *gpf = init_gpf; gpf; gpf = gpf->next) {
			if ((gpf == gpl->actframe) || ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {

				bGPDstroke *gps, *gpsn;

				if (gpf == NULL)
					continue;

				/* simply delete points from selected strokes
				 * NOTE: we may still have to remove the stroke if it ends up having no points!
				 */
				for (gps = gpf->strokes.first; gps; gps = gpsn) {
					gpsn = gps->next;

					/* skip strokes that are invalid for current view */
					if (ED_gpencil_stroke_can_use(C, gps) == false)
						continue;
					/* check if the color is editable */
					if (ED_gpencil_stroke_color_use(gpl, gps) == false)
						continue;

					/* the stroke must have at least one point selected for any operator */
					if (gps->flag & GP_STROKE_SELECT) {
						bGPDspoint *pt;
						int i;

						int tot = gps->totpoints; /* number of points in new buffer */

						/* first pass: count points to remove */
						switch (mode) {
						case GP_DISSOLVE_POINTS:
							/* Count how many points are selected (i.e. how many to remove) */
							for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
								if (pt->flag & GP_SPOINT_SELECT) {
									/* selected point - one of the points to remove */
									tot--;
								}
							}
							break;
						case GP_DISSOLVE_BETWEEN:
							/* need to find first and last point selected */
							first = -1;
							last = 0;
							for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
								if (pt->flag & GP_SPOINT_SELECT) {
									if (first < 0) {
										first = i;
									}
									last = i;
								}
							}
							/* count unselected points in the range */
							for (i = first, pt = gps->points + first; i < last; i++, pt++) {
								if ((pt->flag & GP_SPOINT_SELECT) == 0) {
									tot--;
								}
							}
							break;
						case GP_DISSOLVE_UNSELECT:
							/* count number of unselected points */
							for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
								if ((pt->flag & GP_SPOINT_SELECT) == 0) {
									tot--;
								}
							}
							break;
						default:
							return false;
							break;
						}

						/* if no points are left, we simply delete the entire stroke */
						if (tot <= 0) {
							/* remove the entire stroke */
							if (gps->points) {
								BKE_gpencil_free_stroke_weights(gps);
								MEM_freeN(gps->points);
							}
							if (gps->triangles) {
								MEM_freeN(gps->triangles);
							}
							BLI_freelinkN(&gpf->strokes, gps);
							BKE_gpencil_batch_cache_dirty(gpd);
						}
						else {
							/* just copy all points to keep into a smaller buffer */
							bGPDspoint *new_points = MEM_callocN(sizeof(bGPDspoint) * tot, "new gp stroke points copy");
							bGPDspoint *npt = new_points;

							switch (mode) {
							case GP_DISSOLVE_POINTS:
								for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
									if ((pt->flag & GP_SPOINT_SELECT) == 0) {
										*npt = *pt;
										npt->weights = MEM_dupallocN(pt->weights);
										npt++;
									}
								}
								break;
							case GP_DISSOLVE_BETWEEN:
								/* copy first segment */
								for (i = 0, pt = gps->points; i < first; i++, pt++) {
									*npt = *pt;
									npt->weights = MEM_dupallocN(pt->weights);
									npt++;
								}
								/* copy segment (selected points) */
								for (i = first, pt = gps->points + first; i < last; i++, pt++) {
									if (pt->flag & GP_SPOINT_SELECT) {
										*npt = *pt;
										npt->weights = MEM_dupallocN(pt->weights);
										npt++;
									}
								}
								/* copy last segment */
								for (i = last, pt = gps->points + last; i < gps->totpoints; i++, pt++) {
									*npt = *pt;
									npt->weights = MEM_dupallocN(pt->weights);
									npt++;
								}

								break;
							case GP_DISSOLVE_UNSELECT:
								/* copy any selected point */
								for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
									if (pt->flag & GP_SPOINT_SELECT) {
										*npt = *pt;
										npt->weights = MEM_dupallocN(pt->weights);
										npt++;
									}
								}
								break;
							}

							/* free the old buffer */
							if (gps->points) {
								BKE_gpencil_free_stroke_weights(gps);
								MEM_freeN(gps->points);
							}

							/* save the new buffer */
							gps->points = new_points;
							gps->totpoints = tot;

							/* triangles cache needs to be recalculated */
							gps->flag |= GP_STROKE_RECALC_CACHES;
							gps->tot_triangles = 0;

							/* deselect the stroke, since none of its selected points will still be selected */
							gps->flag &= ~GP_STROKE_SELECT;
							for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
								pt->flag &= ~GP_SPOINT_SELECT;
							}
						}

						changed = true;
					}
				}
			}
		}
	}
	CTX_DATA_END;
	
	if (changed) {
		BKE_gpencil_batch_cache_dirty(gpd);
		WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
		return OPERATOR_FINISHED;
	}
	else {
		return OPERATOR_CANCELLED;
	}
}

/* ----------------------------------- */

/* Temp data for storing information about an "island" of points
 * that should be kept when splitting up a stroke. Used in:
 * gp_stroke_delete_tagged_points()
 */
typedef struct tGPDeleteIsland {
	int start_idx;
	int end_idx;
} tGPDeleteIsland;


/* Split the given stroke into several new strokes, partitioning
 * it based on whether the stroke points have a particular flag
 * is set (e.g. "GP_SPOINT_SELECT" in most cases, but not always)
 *
 * The algorithm used here is as follows:
 * 1) We firstly identify the number of "islands" of non-tagged points
 *    which will all end up being in new strokes.
 *    - In the most extreme case (i.e. every other vert is a 1-vert island),
 *      we have at most n / 2 islands
 *    - Once we start having larger islands than that, the number required
 *      becomes much less
 * 2) Each island gets converted to a new stroke
 */
void gp_stroke_delete_tagged_points(bGPDframe *gpf, bGPDstroke *gps, bGPDstroke *next_stroke, int tag_flags)
{
	tGPDeleteIsland *islands = MEM_callocN(sizeof(tGPDeleteIsland) * (gps->totpoints + 1) / 2, "gp_point_islands");
	bool in_island  = false;
	int num_islands = 0;
	
	
	/* First Pass: Identify start/end of islands */
	bGPDspoint *pt = gps->points;
	for (int i = 0; i < gps->totpoints; i++, pt++) {
		if (pt->flag & tag_flags) {
			/* selected - stop accumulating to island */
			in_island = false;
		}
		else {
			/* unselected - start of a new island? */
			int idx;
			
			if (in_island) {
				/* extend existing island */
				idx = num_islands - 1;
				islands[idx].end_idx = i;
			}
			else {
				/* start of new island */
				in_island = true;
				num_islands++;
				
				idx = num_islands - 1;
				islands[idx].start_idx = islands[idx].end_idx = i;
			}
		}
	}
	
	/* Watch out for special case where No islands = All points selected = Delete Stroke only */
	if (num_islands) {
		/* there are islands, so create a series of new strokes, adding them before the "next" stroke */
		int idx;
		
		/* Create each new stroke... */
		for (idx = 0; idx < num_islands; idx++) {
			tGPDeleteIsland *island = &islands[idx];
			bGPDstroke *new_stroke  = MEM_dupallocN(gps);
			
			/* initialize triangle memory  - to be calculated on next redraw */
			new_stroke->triangles = NULL;
			new_stroke->flag |= GP_STROKE_RECALC_CACHES;
			new_stroke->tot_triangles = 0;
			
			/* Compute new buffer size (+ 1 needed as the endpoint index is "inclusive") */
			new_stroke->totpoints = island->end_idx - island->start_idx + 1;
			new_stroke->points    = MEM_callocN(sizeof(bGPDspoint) * new_stroke->totpoints, "gp delete stroke fragment");

			/* Copy over the relevant points */
			memcpy(new_stroke->points, gps->points + island->start_idx, sizeof(bGPDspoint) * new_stroke->totpoints);
			/* Copy weights */
			int e = island->start_idx;
			for (int i = 0; i < new_stroke->totpoints; ++i) {
				bGPDspoint *pt_src = &gps->points[e];
				bGPDspoint *pt_dst = &new_stroke->points[i];
				pt_dst->weights = MEM_dupallocN(pt_src->weights);
				++e;
			}
			
			/* Each island corresponds to a new stroke. We must adjust the 
			 * timings of these new strokes:
			 *
			 * Each point's timing data is a delta from stroke's inittime, so as we erase some points from
			 * the start of the stroke, we have to offset this inittime and all remaining points' delta values.
			 * This way we get a new stroke with exactly the same timing as if user had started drawing from
			 * the first non-removed point...
			 */
			{
				bGPDspoint *pts;
				float delta = gps->points[island->start_idx].time;
				int j;
				
				new_stroke->inittime += (double)delta;
				
				pts = new_stroke->points;
				for (j = 0; j < new_stroke->totpoints; j++, pts++) {
					pts->time -= delta;
				}
			}
			
			/* Add new stroke to the frame */
			if (next_stroke) {
				BLI_insertlinkbefore(&gpf->strokes, next_stroke, new_stroke);
			}
			else {
				BLI_addtail(&gpf->strokes, new_stroke);
			}
		}
	}
	
	/* free islands */
	MEM_freeN(islands);
	
	/* Delete the old stroke */
	if (gps->points) {
		BKE_gpencil_free_stroke_weights(gps);
		MEM_freeN(gps->points);
	}
	if (gps->triangles) {
		MEM_freeN(gps->triangles);
	}
	BLI_freelinkN(&gpf->strokes, gps);
}

/* Split selected strokes into segments, splitting on selected points */
static int gp_delete_selected_points(bContext *C)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bool is_multiedit = (bool)GPENCIL_MULTIEDIT_SESSIONS_ON(gpd);
	bool changed = false;

	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *init_gpf = gpl->actframe;
		if (is_multiedit) {
			init_gpf = gpl->frames.first;
		}

		for (bGPDframe *gpf = init_gpf; gpf; gpf = gpf->next) {
			if ((gpf == gpl->actframe) || ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {
				bGPDstroke *gps, *gpsn;

				if (gpf == NULL)
					continue;

				/* simply delete strokes which are selected */
				for (gps = gpf->strokes.first; gps; gps = gpsn) {
					gpsn = gps->next;

					/* skip strokes that are invalid for current view */
					if (ED_gpencil_stroke_can_use(C, gps) == false)
						continue;
					/* check if the color is editable */
					if (ED_gpencil_stroke_color_use(gpl, gps) == false)
						continue;


					if (gps->flag & GP_STROKE_SELECT) {
						/* deselect old stroke, since it will be used as template for the new strokes */
						gps->flag &= ~GP_STROKE_SELECT;

						/* delete unwanted points by splitting stroke into several smaller ones */
						gp_stroke_delete_tagged_points(gpf, gps, gpsn, GP_SPOINT_SELECT);

						changed = true;
					}
				}
			}
		}
	}
	CTX_DATA_END;
	
	if (changed) {
		BKE_gpencil_batch_cache_dirty(gpd);
		WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
		return OPERATOR_FINISHED;
	}
	else {
		return OPERATOR_CANCELLED;
	}
}

/* simple wrapper to external call */
int gp_delete_selected_point_wrap(bContext *C)
{
	return gp_delete_selected_points(C);
}

/* ----------------------------------- */

static int gp_delete_exec(bContext *C, wmOperator *op)
{
	eGP_DeleteMode mode = RNA_enum_get(op->ptr, "type");
	int result = OPERATOR_CANCELLED;
	
	switch (mode) {
		case GP_DELETEOP_STROKES:	/* selected strokes */
			result = gp_delete_selected_strokes(C);
			break;
		
		case GP_DELETEOP_POINTS:	/* selected points (breaks the stroke into segments) */
			result = gp_delete_selected_points(C);
			break;
		
		case GP_DELETEOP_FRAME:		/* active frame */
			result = gp_actframe_delete_exec(C, op);
			break;
	}
	
	return result;
}

void GPENCIL_OT_delete(wmOperatorType *ot)
{
	static const EnumPropertyItem prop_gpencil_delete_types[] = {
		{GP_DELETEOP_POINTS, "POINTS", 0, "Points", "Delete selected points and split strokes into segments"},
		{GP_DELETEOP_STROKES, "STROKES", 0, "Strokes", "Delete selected strokes"},
		{GP_DELETEOP_FRAME, "FRAME", 0, "Frame", "Delete active frame"},
		{0, NULL, 0, NULL, NULL}
	};
	
	/* identifiers */
	ot->name = "Delete";
	ot->idname = "GPENCIL_OT_delete";
	ot->description = "Delete selected Grease Pencil strokes, vertices, or frames";
	
	/* callbacks */
	ot->invoke = WM_menu_invoke;
	ot->exec = gp_delete_exec;
	ot->poll = gp_stroke_edit_poll;
	
	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
	
	/* props */
	ot->prop = RNA_def_enum(ot->srna, "type", prop_gpencil_delete_types, 0, "Type", "Method used for deleting Grease Pencil data");
}

static int gp_dissolve_exec(bContext *C, wmOperator *op)
{
	eGP_DissolveMode mode = RNA_enum_get(op->ptr, "type");

	return gp_dissolve_selected_points(C, mode);
}

void GPENCIL_OT_dissolve(wmOperatorType *ot)
{
	static EnumPropertyItem prop_gpencil_dissolve_types[] = {
		{ GP_DISSOLVE_POINTS, "POINTS", 0, "Dissolve", "Dissolve selected points" },
		{ GP_DISSOLVE_BETWEEN, "BETWEEN", 0, "Dissolve Between", "Dissolve points between selected points" },
		{ GP_DISSOLVE_UNSELECT, "UNSELECT", 0, "Dissolve Unselect", "Dissolve all unselected points" },
		{ 0, NULL, 0, NULL, NULL }
	};

	/* identifiers */
	ot->name = "Dissolve";
	ot->idname = "GPENCIL_OT_dissolve";
	ot->description = "Delete selected points without splitting strokes";

	/* callbacks */
	ot->invoke = WM_menu_invoke;
	ot->exec = gp_dissolve_exec;
	ot->poll = gp_stroke_edit_poll;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;

	/* props */
	ot->prop = RNA_def_enum(ot->srna, "type", prop_gpencil_dissolve_types, 0, "Type", "Method used for disolving Stroke points");
}

/* ****************** Snapping - Strokes <-> Cursor ************************ */

/* Poll callback for snap operators */
/* NOTE: For now, we only allow these in the 3D view, as other editors do not
 *       define a cursor or gridstep which can be used
 */
static int gp_snap_poll(bContext *C)
{
	bGPdata *gpd = CTX_data_gpencil_data(C);
	ScrArea *sa = CTX_wm_area(C);
	
	return (gpd != NULL) && ((sa != NULL) && (sa->spacetype == SPACE_VIEW3D));
}

/* --------------------------------- */

static int gp_snap_to_grid(bContext *C, wmOperator *UNUSED(op))
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	RegionView3D *rv3d = CTX_wm_region_data(C);
	Object *obact = CTX_data_active_object(C);
	const float gridf = rv3d->gridview;
	
	for (bGPDlayer *gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		/* only editable and visible layers are considered */
		if (gpencil_layer_is_editable(gpl) && (gpl->actframe != NULL)) {
			bGPDframe *gpf = gpl->actframe;
			float diff_mat[4][4];
			
			/* calculate difference matrix object */
			ED_gpencil_parent_location(obact, gpd, gpl, diff_mat);
			
			for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
				bGPDspoint *pt;
				int i;
				
				/* skip strokes that are invalid for current view */
				if (ED_gpencil_stroke_can_use(C, gps) == false)
					continue;
				/* check if the color is editable */
				if (ED_gpencil_stroke_color_use(gpl, gps) == false)
					continue;
				
				// TODO: if entire stroke is selected, offset entire stroke by same amount?
				for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
					/* only if point is selected */
					if (pt->flag & GP_SPOINT_SELECT) {
						/* apply parent transformations */
						float fpt[3];
						mul_v3_m4v3(fpt, diff_mat, &pt->x);

						fpt[0] = gridf * floorf(0.5f + fpt[0] / gridf);
						fpt[1] = gridf * floorf(0.5f + fpt[1] / gridf);
						fpt[2] = gridf * floorf(0.5f + fpt[2] / gridf);

						/* return data */
						copy_v3_v3(&pt->x, fpt);
						gp_apply_parent_point(obact, gpd, gpl, pt);
					}
				}
			}
		}
	}
	
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_snap_to_grid(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Snap Selection to Grid";
	ot->idname = "GPENCIL_OT_snap_to_grid";
	ot->description = "Snap selected points to the nearest grid points";
	
	/* callbacks */
	ot->exec = gp_snap_to_grid;
	ot->poll = gp_snap_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ------------------------------- */

static int gp_snap_to_cursor(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	
	Scene *scene = CTX_data_scene(C);
	View3D *v3d = CTX_wm_view3d(C);
	Object *obact = CTX_data_active_object(C);                                          \

	const bool use_offset = RNA_boolean_get(op->ptr, "use_offset");
	const float *cursor_global = ED_view3d_cursor3d_get(scene, v3d);
	
	for (bGPDlayer *gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		/* only editable and visible layers are considered */
		if (gpencil_layer_is_editable(gpl) && (gpl->actframe != NULL)) {
			bGPDframe *gpf = gpl->actframe;
			float diff_mat[4][4];
			
			/* calculate difference matrix */
			ED_gpencil_parent_location(obact, gpd, gpl, diff_mat);
			
			for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
				bGPDspoint *pt;
				int i;
				
				/* skip strokes that are invalid for current view */
				if (ED_gpencil_stroke_can_use(C, gps) == false)
					continue;
				/* check if the color is editable */
				if (ED_gpencil_stroke_color_use(gpl, gps) == false)
					continue;
				/* only continue if this stroke is selected (editable doesn't guarantee this)... */
				if ((gps->flag & GP_STROKE_SELECT) == 0)
					continue;
				
				if (use_offset) {
					float offset[3];
					
					/* compute offset from first point of stroke to cursor */
					/* TODO: Allow using midpoint instead? */
					sub_v3_v3v3(offset, cursor_global, &gps->points->x);
					
					/* apply offset to all points in the stroke */
					for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
						add_v3_v3(&pt->x, offset);
					}
				}
				else {
					/* affect each selected point */
					for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
						if (pt->flag & GP_SPOINT_SELECT) {
							copy_v3_v3(&pt->x, cursor_global);
							gp_apply_parent_point(obact, gpd, gpl, pt);
						}
					}
				}
			}
			
		}
	}
	
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_snap_to_cursor(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Snap Selection to Cursor";
	ot->idname = "GPENCIL_OT_snap_to_cursor";
	ot->description = "Snap selected points/strokes to the cursor";
	
	/* callbacks */
	ot->exec = gp_snap_to_cursor;
	ot->poll = gp_snap_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* props */
	ot->prop = RNA_def_boolean(ot->srna, "use_offset", true, "With Offset",
	                           "Offset the entire stroke instead of selected points only");
}

/* ------------------------------- */

static int gp_snap_cursor_to_sel(bContext *C, wmOperator *UNUSED(op))
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	
	Scene *scene = CTX_data_scene(C);
	View3D *v3d = CTX_wm_view3d(C);
	Object *obact = CTX_data_active_object(C);                                          \

	float *cursor = ED_view3d_cursor3d_get(scene, v3d);
	float centroid[3] = {0.0f};
	float min[3], max[3];
	size_t count = 0;
	
	INIT_MINMAX(min, max);
	
	/* calculate midpoints from selected points */
	for (bGPDlayer *gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		/* only editable and visible layers are considered */
		if (gpencil_layer_is_editable(gpl) && (gpl->actframe != NULL)) {
			bGPDframe *gpf = gpl->actframe;
			float diff_mat[4][4];
			
			/* calculate difference matrix */
			ED_gpencil_parent_location(obact, gpd, gpl, diff_mat);
			
			for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
				bGPDspoint *pt;
				int i;
				
				/* skip strokes that are invalid for current view */
				if (ED_gpencil_stroke_can_use(C, gps) == false)
					continue;
				/* check if the color is editable */
				if (ED_gpencil_stroke_color_use(gpl, gps) == false)
					continue;
				/* only continue if this stroke is selected (editable doesn't guarantee this)... */
				if ((gps->flag & GP_STROKE_SELECT) == 0)
					continue;
				
				for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
					if (pt->flag & GP_SPOINT_SELECT) {
						/* apply parent transformations */
						float fpt[3];
						mul_v3_m4v3(fpt, diff_mat, &pt->x);
						
							add_v3_v3(centroid, fpt);
						minmax_v3v3_v3(min, max, fpt);
						
						count++;
					}
				}
				
			}
		}
	}
	
	if (v3d->around == V3D_AROUND_CENTER_MEAN && count) {
		mul_v3_fl(centroid, 1.0f / (float)count);
		copy_v3_v3(cursor, centroid);
	}
	else {
		mid_v3_v3v3(cursor, min, max);
	}

	
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_snap_cursor_to_selected(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Snap Cursor to Selected Points";
	ot->idname = "GPENCIL_OT_snap_cursor_to_selected";
	ot->description = "Snap cursor to center of selected points";
	
	/* callbacks */
	ot->exec = gp_snap_cursor_to_sel;
	ot->poll = gp_snap_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ******************* Apply layer thickness change to strokes ************************** */

static int gp_stroke_apply_thickness_exec(bContext *C, wmOperator *UNUSED(op))
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDlayer *gpl = BKE_gpencil_layer_getactive(gpd);

	/* sanity checks */
	if (ELEM(NULL, gpd, gpl, gpl->frames.first))
		return OPERATOR_CANCELLED;

	/* loop all strokes */
	for (bGPDframe *gpf = gpl->frames.first; gpf; gpf = gpf->next) {
		for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
			/* Apply thickness */
			gps->thickness = gps->thickness + gpl->thickness;
		}
	}
	/* clear value */
	gpl->thickness = 0.0f;

	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_stroke_apply_thickness(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Apply Stroke Thickness";
	ot->idname = "GPENCIL_OT_stroke_apply_thickness";
	ot->description = "Apply the thickness change of the layer to its strokes";

	/* api callbacks */
	ot->exec = gp_stroke_apply_thickness_exec;
	ot->poll = gp_active_layer_poll;
}

/* ******************* Close Strokes ************************** */

enum {
	GP_STROKE_CYCLIC_CLOSE = 1,
	GP_STROKE_CYCLIC_OPEN = 2,
	GP_STROKE_CYCLIC_TOGGLE = 3
};

static int gp_stroke_cyclical_set_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	const int type = RNA_enum_get(op->ptr, "type");
	
	/* sanity checks */
	if (ELEM(NULL, gpd))
		return OPERATOR_CANCELLED;
	
	/* loop all selected strokes */
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		if (gpl->actframe == NULL)
			continue;
			
		for (bGPDstroke *gps = gpl->actframe->strokes.last; gps; gps = gps->prev) {
			PaletteColor *palcolor = gps->palcolor;
			
			/* skip strokes that are not selected or invalid for current view */
			if (((gps->flag & GP_STROKE_SELECT) == 0) || ED_gpencil_stroke_can_use(C, gps) == false)
				continue;
			/* skip hidden or locked colors */
			if (!palcolor || (palcolor->flag & PC_COLOR_HIDE) || (palcolor->flag & PC_COLOR_LOCKED))
				continue;
			
			switch (type) {
				case GP_STROKE_CYCLIC_CLOSE:
					/* Close all (enable) */
					gps->flag |= GP_STROKE_CYCLIC;
					break;
				case GP_STROKE_CYCLIC_OPEN:
					/* Open all (disable) */
					gps->flag &= ~GP_STROKE_CYCLIC;
					break;
				case GP_STROKE_CYCLIC_TOGGLE:
					/* Just toggle flag... */
					gps->flag ^= GP_STROKE_CYCLIC;
					break;
				default:
					BLI_assert(0);
					break;
			}
		}
	}
	CTX_DATA_END;
	
	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

/**
 * Similar to #CURVE_OT_cyclic_toggle or #MASK_OT_cyclic_toggle, but with
 * option to force opened/closed strokes instead of just toggle behavior.
 */
void GPENCIL_OT_stroke_cyclical_set(wmOperatorType *ot)
{
	static const EnumPropertyItem cyclic_type[] = {
		{GP_STROKE_CYCLIC_CLOSE, "CLOSE", 0, "Close all", ""},
		{GP_STROKE_CYCLIC_OPEN, "OPEN", 0, "Open all", ""},
		{GP_STROKE_CYCLIC_TOGGLE, "TOGGLE", 0, "Toggle", ""},
		{0, NULL, 0, NULL, NULL}
	};
	
	/* identifiers */
	ot->name = "Set Cyclical State";
	ot->idname = "GPENCIL_OT_stroke_cyclical_set";
	ot->description = "Close or open the selected stroke adding an edge from last to first point";
	
	/* api callbacks */
	ot->exec = gp_stroke_cyclical_set_exec;
	ot->poll = gp_active_layer_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* properties */
	ot->prop = RNA_def_enum(ot->srna, "type", cyclic_type, GP_STROKE_CYCLIC_TOGGLE, "Type", "");
}

/* ******************* Stroke join ************************** */

/* Helper: flip stroke */
static void gpencil_flip_stroke(bGPDstroke *gps)
{
	int end = gps->totpoints - 1;
	
	for (int i = 0; i < gps->totpoints / 2; i++) {
		bGPDspoint *point, *point2;
		bGPDspoint pt;
	
		/* save first point */
		point = &gps->points[i];
		pt.x = point->x;
		pt.y = point->y;
		pt.z = point->z;
		pt.flag = point->flag;
		pt.pressure = point->pressure;
		pt.strength = point->strength;
		pt.time = point->time;
		
		/* replace first point with last point */
		point2 = &gps->points[end];
		point->x = point2->x;
		point->y = point2->y;
		point->z = point2->z;
		point->flag = point2->flag;
		point->pressure = point2->pressure;
		point->strength = point2->strength;
		point->time = point2->time;
		
		/* replace last point with first saved before */
		point = &gps->points[end];
		point->x = pt.x;
		point->y = pt.y;
		point->z = pt.z;
		point->flag = pt.flag;
		point->pressure = pt.pressure;
		point->strength = pt.strength;
		point->time = pt.time;
		
		end--;
	}
}

/* Helper: copy point between strokes */
static void gpencil_stroke_copy_point(bGPDstroke *gps, bGPDspoint *point, float delta[3],
                                      float pressure, float strength, float deltatime)
{
	bGPDspoint *newpoint;
	
	gps->points = MEM_reallocN(gps->points, sizeof(bGPDspoint) * (gps->totpoints + 1));
	gps->totpoints++;
	
	newpoint = &gps->points[gps->totpoints - 1];
	newpoint->x = point->x * delta[0];
	newpoint->y = point->y * delta[1];
	newpoint->z = point->z * delta[2];
	newpoint->flag = point->flag;
	newpoint->pressure = pressure;
	newpoint->strength = strength;
	newpoint->time = point->time + deltatime;
	newpoint->totweight = point->totweight;
	newpoint->weights = MEM_dupallocN(point->weights);
}

/* Helper: join two strokes using the shortest distance (reorder stroke if necessary ) */
static void gpencil_stroke_join_strokes(bGPDstroke *gps_a, bGPDstroke *gps_b, const bool leave_gaps)
{
	bGPDspoint point;
	bGPDspoint *pt;
	int i;
	float delta[3] = {1.0f, 1.0f, 1.0f};
	float deltatime = 0.0f;
	
	/* sanity checks */
	if (ELEM(NULL, gps_a, gps_b))
		return;
	
	if ((gps_a->totpoints == 0) || (gps_b->totpoints == 0))
		return;
	
	/* define start and end points of each stroke */
	float sa[3], sb[3], ea[3], eb[3];
	pt = &gps_a->points[0];
	copy_v3_v3(sa, &pt->x);
	
	pt = &gps_a->points[gps_a->totpoints - 1];
	copy_v3_v3(ea, &pt->x);
	
	pt = &gps_b->points[0];
	copy_v3_v3(sb, &pt->x);
	
	pt = &gps_b->points[gps_b->totpoints - 1];
	copy_v3_v3(eb, &pt->x);
	
	/* review if need flip stroke B */
	float ea_sb = len_squared_v3v3(ea, sb);
	float ea_eb = len_squared_v3v3(ea, eb);
	/* flip if distance to end point is shorter */
	if (ea_eb < ea_sb) {
		gpencil_flip_stroke(gps_b);
	}
	
	/* don't visibly link the first and last points? */
	if (leave_gaps) {
		/* 1st: add one tail point to start invisible area */
		point = gps_a->points[gps_a->totpoints - 1];
		deltatime = point.time;
		gpencil_stroke_copy_point(gps_a, &point, delta, 0.0f, 0.0f, 0.0f);
		
		/* 2nd: add one head point to finish invisible area */
		point = gps_b->points[0];
		gpencil_stroke_copy_point(gps_a, &point, delta, 0.0f, 0.0f, deltatime);
	}
	
	/* 3rd: add all points */
	for (i = 0, pt = gps_b->points; i < gps_b->totpoints && pt; i++, pt++) {
		/* check if still room in buffer */
		if (gps_a->totpoints <= GP_STROKE_BUFFER_MAX - 2) {
			gpencil_stroke_copy_point(gps_a, pt, delta, pt->pressure, pt->strength, deltatime);
		}
	}
}

static int gp_stroke_join_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDlayer *activegpl = BKE_gpencil_layer_getactive(gpd);
	bGPDstroke *gps, *gpsn;
	Palette *palette = BKE_palette_get_active_from_context(C);
	PaletteColor *palcolor = BKE_palette_color_get_active(palette);

	bGPDframe *gpf_a = NULL;
	bGPDstroke *stroke_a = NULL;
	bGPDstroke *stroke_b = NULL;
	bGPDstroke *new_stroke = NULL;
	
	const int type = RNA_enum_get(op->ptr, "type");
	const bool leave_gaps = RNA_boolean_get(op->ptr, "leave_gaps");
	
	/* sanity checks */
	if (ELEM(NULL, gpd))
		return OPERATOR_CANCELLED;
	
	if (activegpl->flag & GP_LAYER_LOCKED)
		return OPERATOR_CANCELLED;
	
	BLI_assert(ELEM(type, GP_STROKE_JOIN, GP_STROKE_JOINCOPY));
	
	
	/* read all selected strokes */
	bool first = false;
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *gpf = gpl->actframe;
		if (gpf == NULL)
			continue;
		
		for (gps = gpf->strokes.first; gps; gps = gpsn) {
			gpsn = gps->next;
			if (gps->flag & GP_STROKE_SELECT) {
				/* skip strokes that are invalid for current view */
				if (ED_gpencil_stroke_can_use(C, gps) == false) {
					continue;
				}
				/* check if the color is editable */
				if (ED_gpencil_stroke_color_use(gpl, gps) == false) {
					continue;
				}
				
				/* to join strokes, cyclic must be disabled */
				gps->flag &= ~GP_STROKE_CYCLIC;
				
				/* saves first frame and stroke */
				if (!first) {
					first = true;
					gpf_a = gpf;
					stroke_a = gps;
				}
				else {
					stroke_b = gps;
					
					/* create a new stroke if was not created before (only created if something to join) */
					if (new_stroke == NULL) {
						new_stroke = MEM_dupallocN(stroke_a);
						new_stroke->points = MEM_dupallocN(stroke_a->points);
						BKE_gpencil_stroke_weights_duplicate(stroke_a, new_stroke);
						new_stroke->triangles = NULL;
						new_stroke->tot_triangles = 0;
						new_stroke->flag |= GP_STROKE_RECALC_CACHES;
						
						/* if new, set current color */
						if (type == GP_STROKE_JOINCOPY) {
							new_stroke->palcolor = palcolor;
							BLI_strncpy(new_stroke->colorname, palcolor->info, sizeof(new_stroke->colorname));
						}
					}
					
					/* join new_stroke and stroke B. New stroke will contain all the previous data */
					gpencil_stroke_join_strokes(new_stroke, stroke_b, leave_gaps);
					
					/* if join only, delete old strokes */
					if (type == GP_STROKE_JOIN) {
						if (stroke_a) {
							BLI_insertlinkbefore(&gpf_a->strokes, stroke_a, new_stroke);
							BLI_remlink(&gpf->strokes, stroke_a);
							BKE_gpencil_free_stroke(stroke_a);
							stroke_a = NULL;
						}
						if (stroke_b) {
							BLI_remlink(&gpf->strokes, stroke_b);
							BKE_gpencil_free_stroke(stroke_b);
							stroke_b = NULL;
						}
					}
				}
			}
		}
	}
	CTX_DATA_END;
	
	/* add new stroke if was not added before */
	if (type == GP_STROKE_JOINCOPY) {
		if (new_stroke) {
			/* Add a new frame if needed */
			if (activegpl->actframe == NULL)
				activegpl->actframe = BKE_gpencil_frame_addnew(activegpl, gpf_a->framenum);
			
			BLI_addtail(&activegpl->actframe->strokes, new_stroke);
		}
	}
	
	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_stroke_join(wmOperatorType *ot)
{
	static const EnumPropertyItem join_type[] = {
		{GP_STROKE_JOIN, "JOIN", 0, "Join", ""},
		{GP_STROKE_JOINCOPY, "JOINCOPY", 0, "Join and Copy", ""},
		{0, NULL, 0, NULL, NULL}
	};
	
	/* identifiers */
	ot->name = "Join Strokes";
	ot->idname = "GPENCIL_OT_stroke_join";
	ot->description = "Join selected strokes (optionally as new stroke)";
	
	/* api callbacks */
	ot->exec = gp_stroke_join_exec;
	ot->poll = gp_active_layer_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* properties */
	ot->prop = RNA_def_enum(ot->srna, "type", join_type, GP_STROKE_JOIN, "Type", "");
	RNA_def_boolean(ot->srna, "leave_gaps", false, "Leave Gaps", "Leave gaps between joined strokes instead of linking them");
}

/* ******************* Stroke flip ************************** */

static int gp_stroke_flip_exec(bContext *C, wmOperator *UNUSED(op))
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);

	/* sanity checks */
	if (ELEM(NULL, gpd))
		return OPERATOR_CANCELLED;

	/* read all selected strokes */
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *gpf = gpl->actframe;
		if (gpf == NULL)
			continue;
			
		for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
			if (gps->flag & GP_STROKE_SELECT) {
				/* skip strokes that are invalid for current view */
				if (ED_gpencil_stroke_can_use(C, gps) == false) {
					continue;
				}
				/* check if the color is editable */
				if (ED_gpencil_stroke_color_use(gpl, gps) == false) {
					continue;
				}
				
				/* flip stroke */
				gpencil_flip_stroke(gps);
			}
		}
	}
	CTX_DATA_END;
	
	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_stroke_flip(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Flip Stroke";
	ot->idname = "GPENCIL_OT_stroke_flip";
	ot->description = "Change direction of the points of the selected strokes";
	
	/* api callbacks */
	ot->exec = gp_stroke_flip_exec;
	ot->poll = gp_active_layer_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ***************** Reproject Strokes ********************** */

typedef enum eGP_ReprojectModes {
	/* Axis (equal to lock axis) */
	GP_REPROJECT_AXIS = 0,
	/* On same plane, parallel to viewplane */
	GP_REPROJECT_PLANAR,
	/* Reprojected on to the scene geometry */
	GP_REPROJECT_SURFACE,
} eGP_ReprojectModes;

static int gp_strokes_reproject_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	Scene *scene = CTX_data_scene(C);
	ToolSettings *ts = CTX_data_tool_settings(C);
	Object *ob = CTX_data_active_object(C);
	ScrArea *sa = CTX_wm_area(C);
	ARegion *ar = CTX_wm_region(C);
 	RegionView3D *rv3d = ar->regiondata;
	View3D *v3d = sa->spacedata.first;

	GP_SpaceConversion gsc = {NULL};
	eGP_ReprojectModes mode = RNA_enum_get(op->ptr, "type");
	
	int lock_axis = ts->gp_sculpt.lock_axis;
	float origin[3];

	if ((mode == GP_REPROJECT_AXIS) && (lock_axis == GP_LOCKAXIS_NONE)) {
		BKE_report(op->reports, RPT_ERROR, "To reproject by axis, a lock axis must be set before");
		return OPERATOR_CANCELLED;
	}

	/* init space conversion stuff */
	gp_point_conversion_init(C, &gsc);

	/* init autodist for geometry projection */
	if (mode == GP_REPROJECT_SURFACE) {
		EvaluationContext eval_ctx;
		CTX_data_eval_ctx(C, &eval_ctx);

		struct Depsgraph *graph = CTX_data_depsgraph(C);
		view3d_region_operator_needs_opengl(CTX_wm_window(C), gsc.ar);
		ED_view3d_autodist_init(&eval_ctx, graph, gsc.ar, CTX_wm_view3d(C), 0);
	}
	
	// TODO: For deforming geometry workflow, create new frames?
	
	/* Go through each editable + selected stroke, adjusting each of its points one by one... */
	GP_EDITABLE_STROKES_BEGIN(C, gpl, gps)
	{
		if (gps->flag & GP_STROKE_SELECT) {
			bGPDspoint *pt;
			int i;
			float inverse_diff_mat[4][4];
			
			/* Compute inverse matrix for unapplying parenting once instead of doing per-point */
			/* TODO: add this bit to the iteration macro? */
			invert_m4_m4(inverse_diff_mat, diff_mat);

			/* Adjust each point */
			for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
				float xy[2];
				
				/* 3D to Screenspace */
				/* Note: We can't use gp_point_to_xy() here because that uses ints for the screenspace
				 *       coordinates, resulting in lost precision, which in turn causes stairstepping
				 *       artifacts in the final points.
				 */
				bGPDspoint pt2;
				gp_point_to_parent_space(pt, diff_mat, &pt2);
				gp_point_to_xy_fl(&gsc, gps, &pt2, &xy[0], &xy[1]);
				
				/* Project stroke in the axis locked */
				if (mode == GP_REPROJECT_AXIS) {
					if (lock_axis > GP_LOCKAXIS_NONE) {
						ED_gp_get_drawing_reference(v3d, scene, ob, gpl,
							ts->gpencil_v3d_align, origin);
						ED_gp_project_point_to_plane(ob, rv3d, origin, 
							lock_axis - 1, ts->gpencil_src, &pt2);
						
						copy_v3_v3(&pt->x, &pt2.x);

						/* apply parent again */
						gp_apply_parent_point(ob, gpd, gpl, pt);
					}
				}
				/* Project screenspace back to 3D space (from current perspective)
				 * so that all points have been treated the same way
				 */
				else if (mode == GP_REPROJECT_PLANAR) {
					/* Planar - All on same plane parallel to the viewplane */
					gp_point_xy_to_3d(&gsc, scene, xy, &pt->x);
				}
				else {
					/* Geometry - Snap to surfaces of visible geometry */
					/* XXX: There will be precision loss (possible stairstep artifacts) from this conversion to satisfy the API's */
					const int screen_co[2] = {(int)xy[0], (int)xy[1]};
					
					int depth_margin = 0; // XXX: 4 for strokes, 0 for normal
					float depth;
					
					/* XXX: The proper procedure computes the depths into an array, to have smooth transitions when all else fails... */
					if (ED_view3d_autodist_depth(gsc.ar, screen_co, depth_margin, &depth)) {
						ED_view3d_autodist_simple(gsc.ar, screen_co, &pt->x, 0, &depth);
					}
					else {
						/* Default to planar */
						gp_point_xy_to_3d(&gsc, scene, xy, &pt->x);
					}
				}
				
				/* Unapply parent corrections */
				if (mode != GP_REPROJECT_AXIS) {
					mul_m4_v3(inverse_diff_mat, &pt->x);
				}
			}
		}
	}
	GP_EDITABLE_STROKES_END;
	
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
	return OPERATOR_FINISHED;
}

void GPENCIL_OT_reproject(wmOperatorType *ot)
{
	static const EnumPropertyItem reproject_type[] = {
		{ GP_REPROJECT_AXIS, "AXIS", 0, "Axis",
		"Reproject the strokes using the current lock axis configuration. This is the same projection using while"
		"drawing new strokes" },
		{GP_REPROJECT_PLANAR, "PLANAR", 0, "Planar",
		 "Reproject the strokes to end up on the same plane, as if drawn from the current viewpoint "
		 "using 'Cursor' Stroke Placement"},
		{GP_REPROJECT_SURFACE, "SURFACE", 0, "Surface",
		 "Reproject the strokes on to the scene geometry, as if drawn using 'Surface' placement"},
		{0, NULL, 0, NULL, NULL}
	};
	
	/* identifiers */
	ot->name = "Reproject Strokes";
	ot->idname = "GPENCIL_OT_reproject";
	ot->description = "Reproject the selected strokes from the current viewpoint as if they had been newly drawn "
	                  "(e.g. to fix problems from accidental 3D cursor movement or accidental viewport changes, "
	                  "or for matching deforming geometry)";
	
	/* callbacks */
	ot->invoke = WM_menu_invoke;
	ot->exec = gp_strokes_reproject_exec;
	ot->poll = gp_strokes_edit3d_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* properties */
	ot->prop = RNA_def_enum(ot->srna, "type", reproject_type, GP_REPROJECT_PLANAR, "Projection Type", "");
}

/* ******************* Stroke subdivide ************************** */

/* helper: Count how many points need to be inserted */
static int gp_count_subdivision_cuts(bGPDstroke *gps)
{
	bGPDspoint *pt;
	int i;
	int totnewpoints = 0;
	for (i = 0, pt = gps->points; i < gps->totpoints && pt; i++, pt++) {
		if (pt->flag & GP_SPOINT_SELECT) {
			if (i + 1 < gps->totpoints) {
				if (gps->points[i + 1].flag & GP_SPOINT_SELECT) {
					++totnewpoints;
				}
			}
		}
	}

	return totnewpoints;
}
static int gp_stroke_subdivide_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDspoint *temp_points;
	const int cuts = RNA_int_get(op->ptr, "number_cuts");

	int totnewpoints, oldtotpoints;
	int i2;

	/* sanity checks */
	if (ELEM(NULL, gpd))
		return OPERATOR_CANCELLED;

	/* Go through each editable + selected stroke */
	GP_EDITABLE_STROKES_BEGIN(C, gpl, gps)
	{
		if (gps->flag & GP_STROKE_SELECT) {
			/* loop as many times as cuts */
			for (int s = 0; s < cuts; s++) {
				totnewpoints = gp_count_subdivision_cuts(gps);
				if (totnewpoints == 0) {
					continue;
				}
				/* duplicate points in a temp area */
				temp_points = MEM_dupallocN(gps->points);
				oldtotpoints = gps->totpoints;
// TODO
				/* resize the points arrys */
				gps->totpoints += totnewpoints;
				gps->points = MEM_recallocN(gps->points, sizeof(*gps->points) * gps->totpoints);
				gps->flag |= GP_STROKE_RECALC_CACHES;

				/* loop and interpolate */
				i2 = 0;
				for (int i = 0; i < oldtotpoints; i++) {
					bGPDspoint *pt = &temp_points[i];
					bGPDspoint *pt_final = &gps->points[i2];

					/* copy current point */
					copy_v3_v3(&pt_final->x, &pt->x);
					pt_final->pressure = pt->pressure;
					pt_final->strength = pt->strength;
					pt_final->time = pt->time;
					pt_final->flag = pt->flag;
					pt_final->totweight = 0;
					pt_final->weights = NULL;
					++i2;

					/* if next point is selected add a half way point */
					if (pt->flag & GP_SPOINT_SELECT) {
						if (i + 1 < oldtotpoints) {
							if (temp_points[i + 1].flag & GP_SPOINT_SELECT) {
								pt_final = &gps->points[i2];
								/* Interpolate all values */
								bGPDspoint *next = &temp_points[i + 1];
								interp_v3_v3v3(&pt_final->x, &pt->x, &next->x, 0.5f);
								pt_final->pressure = interpf(pt->pressure, next->pressure, 0.5f);
								pt_final->strength = interpf(pt->strength, next->strength, 0.5f);
								CLAMP(pt_final->strength, GPENCIL_STRENGTH_MIN, 1.0f);
								pt_final->time = interpf(pt->time, next->time, 0.5f);
								pt_final->flag |= GP_SPOINT_SELECT;
								pt_final->totweight = 0;
								pt_final->weights = NULL;

								++i2;
							}
						}
					}
				}
				/* free temp memory */
				MEM_freeN(temp_points);
			}
		}
	}
	GP_EDITABLE_STROKES_END;

	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_stroke_subdivide(wmOperatorType *ot)
{
	PropertyRNA *prop;

	/* identifiers */
	ot->name = "Subdivide Stroke";
	ot->idname = "GPENCIL_OT_stroke_subdivide";
	ot->description = "Subdivide between continuous selected points of the stroke adding a point half way between them";

	/* api callbacks */
	ot->exec = gp_stroke_subdivide_exec;
	ot->poll = gp_active_layer_poll;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* properties */
	prop = RNA_def_int(ot->srna, "number_cuts", 1, 1, 10, "Number of Cuts", "", 1, 5);
	/* avoid re-using last var because it can cause _very_ high value and annoy users */
	RNA_def_property_flag(prop, PROP_SKIP_SAVE);

}

/* ** simplify stroke *** */
static int gp_stroke_simplify_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	float factor = RNA_float_get(op->ptr, "factor");

	/* sanity checks */
	if (ELEM(NULL, gpd))
		return OPERATOR_CANCELLED;

	/* Go through each editable + selected stroke */
	GP_EDITABLE_STROKES_BEGIN(C, gpl, gps)
	{
		if (gps->flag & GP_STROKE_SELECT) {
			/* simplify stroke using Ramer-Douglas-Peucker algorithm */
			BKE_gpencil_simplify_stroke(gpl, gps, factor);
		}
	}
	GP_EDITABLE_STROKES_END;

	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_stroke_simplify(wmOperatorType *ot)
{
	PropertyRNA *prop;

	/* identifiers */
	ot->name = "Simplify Stroke";
	ot->idname = "GPENCIL_OT_stroke_simplify";
	ot->description = "Simplify selected stroked reducing number of points";

	/* api callbacks */
	ot->exec = gp_stroke_simplify_exec;
	ot->poll = gp_active_layer_poll;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* properties */
	prop = RNA_def_float(ot->srna, "factor", 0.0f, 0.0f, 100.0f, "Factor", "", 0.0f, 100.0f);
	/* avoid re-using last var */
	RNA_def_property_flag(prop, PROP_SKIP_SAVE);

}

/* ** simplify stroke using fixed algorith *** */
static int gp_stroke_simplify_fixed_exec(bContext *C, wmOperator *op)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	int steps = RNA_int_get(op->ptr, "step");

	/* sanity checks */
	if (ELEM(NULL, gpd))
		return OPERATOR_CANCELLED;

	/* Go through each editable + selected stroke */
	GP_EDITABLE_STROKES_BEGIN(C, gpl, gps)
	{
		if (gps->flag & GP_STROKE_SELECT) {
			for (int i = 0; i < steps; i++) {
				BKE_gpencil_simplify_fixed(gpl, gps);
			}
		}
	}
	GP_EDITABLE_STROKES_END;

	/* notifiers */
	BKE_gpencil_batch_cache_dirty(gpd);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_stroke_simplify_fixed(wmOperatorType *ot)
{
	PropertyRNA *prop;

	/* identifiers */
	ot->name = "Simplify Fixed Stroke";
	ot->idname = "GPENCIL_OT_stroke_simplify_fixed";
	ot->description = "Simplify selected stroked reducing number of points using fixed algorithm";

	/* api callbacks */
	ot->exec = gp_stroke_simplify_fixed_exec;
	ot->poll = gp_active_layer_poll;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* properties */
	prop = RNA_def_int(ot->srna, "step", 1, 1, 100, "Steps", "Number of simplify steps", 1, 10);
	
	/* avoid re-using last var */
	RNA_def_property_flag(prop, PROP_SKIP_SAVE);

}

/* ***************** Separate Strokes ********************** */
typedef enum eGP_SeparateModes {
	/* Selected Strokes */
	GP_SEPARATE_SELECT = 0,
	/* Current Layer */
	GP_SEPARATE_LAYER,
} eGP_SeparateModes;

static int gp_stroke_separate_exec(bContext *C, wmOperator *op)
{
	Base *base_new;
	Main *bmain = CTX_data_main(C);
	Scene *scene = CTX_data_scene(C);
	ViewLayer *view_layer = CTX_data_view_layer(C);
	Base *base_old = CTX_data_active_base(C);
	bGPdata *src_gpd = ED_gpencil_data_get_active(C);
	Object *src_ob = CTX_data_active_object(C);
	Object *dst_ob = NULL;
	bGPdata *dst_gpd = NULL;

	eGP_SeparateModes mode = RNA_enum_get(op->ptr, "mode");

	/* sanity checks */
	if (ELEM(NULL, src_ob, src_gpd)) {
		return OPERATOR_CANCELLED;
	}
	bool is_multiedit = (bool)GPENCIL_MULTIEDIT_SESSIONS_ON(src_gpd);

	/* create a new object */
	base_new = ED_object_add_duplicate(bmain, scene, view_layer, base_old, 0);
	dst_ob = base_new->object;

	/* create new grease pencil datablock and copy paletteslots */
	dst_gpd = BKE_gpencil_data_addnew(bmain, "GPencil");
	dst_ob->data = (bGPdata *)dst_gpd;
	BKE_gpencil_copy_palette_data(dst_gpd, src_gpd);

	/* loop old datablock and separate parts */
	bGPDlayer *dst_gpl = NULL;
	bGPDframe *dst_gpf = NULL;
	if (mode == GP_SEPARATE_SELECT) {
		CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
		{
			dst_gpl = NULL;
			bGPDframe *init_gpf = gpl->actframe;
			if (is_multiedit) {
				init_gpf = gpl->frames.first;
			}

			for (bGPDframe *gpf = init_gpf; gpf; gpf = gpf->next) {
				if ((gpf == gpl->actframe) || ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {
					bGPDstroke *gps, *gpsn;

					if (gpf == NULL) {
						continue;
					}

					dst_gpf = NULL;

					for (gps = gpf->strokes.first; gps; gps = gpsn) {
						gpsn = gps->next;

						/* skip strokes that are invalid for current view */
						if (ED_gpencil_stroke_can_use(C, gps) == false) {
							continue;
						}
						/* check if the color is editable */
						if (ED_gpencil_stroke_color_use(gpl, gps) == false) {
							continue;
						}
						/*  separate selected strokes */
						if (gps->flag & GP_STROKE_SELECT) {
							/* add layer if not created before */
							if (dst_gpl == NULL) {
								dst_gpl = BKE_gpencil_layer_addnew(dst_gpd, gpl->info, false);
							}

							/* add frame if not created before */
							if (dst_gpf == NULL) {
								dst_gpf = BKE_gpencil_layer_getframe(dst_gpl, gpf->framenum, GP_GETFRAME_ADD_NEW);
							}
							/* deselect old stroke */
							gps->flag &= ~GP_STROKE_SELECT;
							/* unlink from source frame */
							BLI_remlink(&gpf->strokes, gps);
							gps->prev = gps->next = NULL;
							/* relink to destination frame */
							BLI_addtail(&dst_gpf->strokes, gps);
						}
					}
				}

				/* if not multiedit, exit loop*/
				if (!is_multiedit) {
					break;
				}
			}
		}
		CTX_DATA_END;
	}
	else if (mode == GP_SEPARATE_LAYER) {
		bGPDlayer *gpl = CTX_data_active_gpencil_layer(C);
		if (gpl) {
			/* try to set a new active layer in source datablock */
			if (gpl->prev) {
				BKE_gpencil_layer_setactive(src_gpd, gpl->prev);
			}
			else if (gpl->next) {
				BKE_gpencil_layer_setactive(src_gpd, gpl->next);
			}
			/* unlink from source datablock */
			BLI_remlink(&src_gpd->layers, gpl);
			gpl->prev = gpl->next = NULL;
			/* relink to destination datablock */
			BLI_addtail(&dst_gpd->layers, gpl);
		}
	}

	BKE_gpencil_batch_cache_dirty(src_gpd);
	BKE_gpencil_batch_cache_dirty(dst_gpd);

	DEG_relations_tag_update(bmain);
	WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, NULL);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_stroke_separate(wmOperatorType *ot)
{
	static const EnumPropertyItem separate_type[] = {
	{GP_SEPARATE_SELECT, "SELECT", 0, "Selected Strokes", "Separate the selected strokes"},
	{ GP_SEPARATE_LAYER, "LAYER", 0, "Active Layer", "Separate the strokes of the current layer" },
	{ 0, NULL, 0, NULL, NULL }
	};

	/* identifiers */
	ot->name = "Separate Strokes";
	ot->idname = "GPENCIL_OT_stroke_separate";
	ot->description = "Separate the selected strokes or layer in a new grease pencil object";

	/* callbacks */
	ot->invoke = WM_menu_invoke;
	ot->exec = gp_stroke_separate_exec;
	ot->poll = gp_strokes_edit3d_poll;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* properties */
	ot->prop = RNA_def_enum(ot->srna, "mode", separate_type, GP_SEPARATE_SELECT, "Mode", "");
}
