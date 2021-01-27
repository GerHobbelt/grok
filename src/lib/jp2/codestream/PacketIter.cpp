/*
 *    Copyright (C) 2016-2021 Grok Image Compression Inc.
 *
 *    This source code is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This source code is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 *    This source code incorporates work covered by the BSD 2-clause license.
 *    Please see the LICENSE file in the root directory for details.
 *
 */

#include "grk_includes.h"

namespace grk {

/** @defgroup PI PI - Implementation of a packet iterator */
/*@{*/

/** @name Local static functions */
/*@{*/


/**
 * Updates the coding parameters
 *
 * @param	p_cp		the coding parameters to modify
 * @param	num_comps		the number of components
 * @param	tileno	the tile index being concerned.
 * @param	tileBounds tile bounds
 * @param	max_precincts	the maximum number of precincts for all the bands of the tile
 * @param	max_res	the maximum number of resolutions for all the poc inside the tile.
 * @param	dx_min		the minimum dx of all the components of all the resolutions for the tile.
 * @param	dy_min		the minimum dy of all the components of all the resolutions for the tile.
 */
static void pi_update_compress(CodingParams *p_cp,
									uint16_t num_comps,
									uint16_t tileno,
									grk_rect_u32 tileBounds,
									uint64_t max_precincts,
									uint8_t max_res,
									uint32_t dx_min,
									uint32_t dy_min,
									bool poc);

/**
 * Gets the compression parameters needed to update the coding parameters and all the pocs.
 * The precinct widths, heights, dx and dy for each component at each resolution will be stored as well.
 * the last parameter of the function should be an array of pointers of size nb components, each pointer leading
 * to an area of size 4 * max_res. The data is stored inside this area with the following pattern :
 * dx_compi_res0 , dy_compi_res0 , w_compi_res0, h_compi_res0 , dx_compi_res1 , dy_compi_res1 , w_compi_res1, h_compi_res1 , ...
 *
 * @param	p_image		the image being encoded.
 * @param	p_cp		the coding parameters.
 * @param	tileno		the tile index of the tile being encoded.
 * @param	tileBounds	tile bounds
 * @param	max_precincts	maximum number of precincts for all the bands of the tile
 * @param	max_res		maximum number of resolutions for all the poc inside the tile.
 * @param	dx_min		minimum dx of all the components of all the resolutions for the tile.
 * @param	dy_min		minimum dy of all the components of all the resolutions for the tile.
 * @param	p_resolutions	pointer to an area corresponding to the one described above.
 */
static void pi_get_encoding_params(const grk_image *image,
											const CodingParams *p_cp,
											uint16_t tileno,
											grk_rect_u32 *tileBounds,
											uint32_t *dx_min,
											uint32_t *dy_min,
											uint64_t *precincts,
											uint64_t *max_precincts,
											uint8_t *max_res,
											uint32_t **p_resolutions);
/**
 * Allocates memory for a packet iterator. Data and data sizes are set by this operation.
 * No other data is set. The include section of the packet  iterator is not allocated.
 *
 * @param	p_image		the image used to initialize the packet iterator (in fact only the number of components is relevant.
 * @param	p_cp		the coding parameters.
 * @param	tileno	the index of the tile from which creating the packet iterator.
 */
static PacketIter* pi_create(const grk_image *p_image,
								const CodingParams *p_cp,
								uint16_t tileno,
								IncludeTracker *include);
/**
* Check if there is a remaining valid progression order
 */
static bool pi_check_next_for_valid_progression(int32_t prog,
												CodingParams *cp,
												uint16_t tileno,
												uint32_t pino,
												const char *progString);


/*@}*/

/*@}*/

/*
 ==========================================================
 local functions
 ==========================================================
 */
static void pi_get_encoding_params(const grk_image *image,
											const CodingParams *p_cp,
											uint16_t tileno,
											grk_rect_u32 *tileBounds,
											uint32_t *dx_min,
											uint32_t *dy_min,
											uint64_t *precincts,
											uint64_t *max_precincts,
											uint8_t *max_res,
											uint32_t **p_resolutions) {
	assert(p_cp != nullptr);
	assert(image != nullptr);
	assert(tileno < p_cp->t_grid_width * p_cp->t_grid_height);

	uint32_t tile_x = tileno % p_cp->t_grid_width;
	uint32_t tile_y = tileno / p_cp->t_grid_width;
	*tileBounds = p_cp->getTileBounds(image, tile_x, tile_y);

	*max_precincts = 0;
	*max_res = 0;
	*dx_min = UINT_MAX;
	*dy_min = UINT_MAX;

	for (uint32_t i = 0; i < GRK_J2K_MAXRLVLS; ++i)
		precincts[i] = 0;
	auto tcp = &p_cp->tcps[tileno];
	for (uint32_t compno = 0; compno < image->numcomps; ++compno) {
		auto resolution = p_resolutions[compno];
		auto tccp = tcp->tccps + compno;
		auto comp = image->comps + compno;

		auto tileCompBounds = tileBounds->rectceildiv(comp->dx,comp->dy);
		if (tccp->numresolutions > *max_res)
			*max_res = tccp->numresolutions;

		/* use custom size for precincts*/
		uint8_t level_no = (uint8_t)(tccp->numresolutions - 1);
		for (uint32_t resno = 0; resno < tccp->numresolutions; ++resno) {
			uint32_t pdx = tccp->prcw_exp[resno];
			uint32_t pdy = tccp->prch_exp[resno];
			*resolution++ = pdx;
			*resolution++ = pdy;

			uint64_t dx = comp->dx * ((uint64_t) 1u << (pdx + level_no));
			uint64_t dy = comp->dy * ((uint64_t) 1u << (pdy + level_no));
			if (dx < UINT_MAX)
				*dx_min = std::min<uint32_t>(*dx_min, (uint32_t) dx);
			if (dy < UINT_MAX)
				*dy_min = std::min<uint32_t>(*dy_min, (uint32_t) dy);
			auto resBounds 	= tileCompBounds.rectceildivpow2(level_no);
			uint32_t px0 	= uint_floordivpow2(resBounds.x0, pdx) << pdx;
			uint32_t py0 	= uint_floordivpow2(resBounds.y0, pdy) << pdy;
			uint32_t px1 	= ceildivpow2<uint32_t>(resBounds.x1, pdx) << pdx;
			uint32_t py1 	= ceildivpow2<uint32_t>(resBounds.y1, pdy) << pdy;
			uint32_t pw 	= (resBounds.width()==0) ? 0 : ((px1 - px0) >> pdx);
			uint32_t ph 	= (resBounds.height()==0) ? 0 : ((py1 - py0) >> pdy);
			*resolution++ = pw;
			*resolution++ = ph;
			uint64_t product = (uint64_t)pw * ph;
			if (product > precincts[resno])
				precincts[resno] = product;
			if (product > *max_precincts)
				*max_precincts = product;
			--level_no;
		}
	}
}

static PacketIter* pi_create(const grk_image *image,
							const CodingParams *cp,
							uint16_t tileno,
							IncludeTracker *include) {
	assert(cp != nullptr);
	assert(image != nullptr);
	assert(tileno < cp->t_grid_width * cp->t_grid_height);
	auto tcp = &cp->tcps[tileno];
	uint32_t poc_bound = tcp->numpocs + 1;
	auto pi = new PacketIter[poc_bound];

	for (uint32_t i = 0; i < poc_bound; ++i)
		pi[i].includeTracker = include;
	for (uint32_t pino = 0; pino < poc_bound; ++pino) {
		auto current_pi = pi + pino;
		current_pi->comps = (grk_pi_comp*) grk_calloc(image->numcomps,
				sizeof(grk_pi_comp));
		if (!current_pi->comps) {
			pi_destroy(pi);
			return nullptr;
		}
		current_pi->numcomps = image->numcomps;
		for (uint32_t compno = 0; compno < image->numcomps; ++compno) {
			auto comp = current_pi->comps + compno;
			auto tccp = &tcp->tccps[compno];
			comp->resolutions = (grk_pi_resolution*) grk_calloc(tccp->numresolutions, sizeof(grk_pi_resolution));
			if (!comp->resolutions) {
				pi_destroy(pi);
				return nullptr;
			}
			comp->numresolutions = tccp->numresolutions;
		}
	}
	return pi;
}

static void pi_update_compress(CodingParams *p_cp,
									uint16_t num_comps,
									uint16_t tileno,
									grk_rect_u32 tileBounds,
									uint64_t max_precincts,
									uint8_t max_res,
									uint32_t dx_min,
									uint32_t dy_min,
									bool poc) {
	assert(p_cp != nullptr);
	assert(tileno < p_cp->t_grid_width * p_cp->t_grid_height);
	auto tcp = &p_cp->tcps[tileno];
	for (uint32_t pino = 0; pino <= tcp->numpocs; ++pino) {
		auto current_poc 		= tcp->pocs + pino;
		current_poc->prg 		= poc ? current_poc->prg1 : tcp->prg;
		current_poc->tpLayE 	= poc ? current_poc->layE : tcp->numlayers;
		current_poc->tpResS 	= poc ? current_poc->resS : 0;
		current_poc->tpResE 	= poc ? current_poc->resE : max_res;
		current_poc->tpCompS 	= poc ? current_poc->compS : 0;
		current_poc->tpCompE 	= poc ? current_poc->compE : num_comps;
		current_poc->tpPrecE 	= max_precincts;
		current_poc->tp_txS 	= tileBounds.x0;
		current_poc->tp_txE 	= tileBounds.x1;
		current_poc->tp_tyS 	= tileBounds.y0;
		current_poc->tp_tyE 	= tileBounds.y1;
		current_poc->dx 		= dx_min;
		current_poc->dy 		= dy_min;
	}
}

/**
 * Check if there is a remaining valid progression order
 */
static bool pi_check_next_for_valid_progression(int32_t prog,
												CodingParams *cp,
												uint16_t tileno,
												uint32_t pino,
												const char *progString) {
	auto tcps = cp->tcps + tileno;
	auto poc = tcps->pocs + pino;

	if (prog >= 0) {
		switch (progString[prog]) {
		case 'R':
			if (poc->res_temp == poc->tpResE)
				return pi_check_next_for_valid_progression(prog - 1, cp, tileno, pino, progString);
			else
				return true;
			break;
		case 'C':
			if (poc->comp_temp == poc->tpCompE)
				return pi_check_next_for_valid_progression(prog - 1, cp, tileno, pino, progString);
			else
				return true;
			break;
		case 'L':
			if (poc->lay_temp == poc->tpLayE)
				return pi_check_next_for_valid_progression(prog - 1, cp, tileno, pino, progString);
			else
				return true;
			break;
		case 'P':
			switch (poc->prg) {
			case GRK_LRCP: /* fall through */
			case GRK_RLCP:
				if (poc->prec_temp == poc->tpPrecE)
					return pi_check_next_for_valid_progression(prog - 1, cp, tileno, pino,	progString);
				else
					return true;
				break;
			default:
				if (poc->tx0_temp == poc->tp_txE) {
					/*TY*/
					if (poc->ty0_temp == poc->tp_tyE)
						return pi_check_next_for_valid_progression(prog - 1, cp, tileno, pino,	progString);
					else
						return true;
					/*TY*/
				} else {
					return true;
				}
				break;
			}/*end case P*/
		}/*end switch*/
	}/*end if*/
	return false;
}

/*
 ==========================================================
 Packet iterator interface
 ==========================================================
 */
PacketIter* pi_create_decompress(grk_image *image,
								CodingParams *p_cp,
								uint16_t tile_no,
								IncludeTracker *include) {
	assert(p_cp != nullptr);
	assert(image != nullptr);
	assert(tile_no < p_cp->t_grid_width * p_cp->t_grid_height);

	auto tcp = &p_cp->tcps[tile_no];
	uint32_t bound = tcp->numpocs + 1;

	uint32_t data_stride = 4 * GRK_J2K_MAXRLVLS;
	auto tmp_data = (uint32_t*) grk_malloc((size_t)data_stride * image->numcomps * sizeof(uint32_t));
	if (!tmp_data)
		return nullptr;
	auto tmp_ptr = (uint32_t**) grk_malloc(image->numcomps * sizeof(uint32_t*));
	if (!tmp_ptr) {
		grk_free(tmp_data);
		return nullptr;
	}

	/* memory allocation for pi */
	auto pi = pi_create(image, p_cp, tile_no, include);
	if (!pi) {
		grk_free(tmp_data);
		grk_free(tmp_ptr);
		return nullptr;
	}

	auto encoding_value_ptr = tmp_data;
	/* update pointer array */
	for (uint32_t compno = 0; compno < image->numcomps; ++compno) {
		tmp_ptr[compno] = encoding_value_ptr;
		encoding_value_ptr += data_stride;
	}

	uint8_t max_res;
	uint64_t max_precincts;
	grk_rect_u32 tileBounds;
	uint32_t dx_min, dy_min;
	pi_get_encoding_params(image,
							p_cp,
							tile_no,
							&tileBounds,
							&dx_min,
							&dy_min,
							include->precincts,
							&max_precincts,
							&max_res,
							tmp_ptr);

	/* step calculations */
	uint32_t step_p = 1;
	uint64_t step_c = max_precincts * step_p;
	uint64_t step_r = image->numcomps * step_c;
	uint64_t step_l = max_res * step_r;

	/* set values for first packet iterator */
	auto current_pi = pi;

	/* special treatment for the first packet iterator */
	current_pi->tx0 = tileBounds.x0;
	current_pi->ty0 = tileBounds.y0;
	current_pi->tx1 = tileBounds.x1;
	current_pi->ty1 = tileBounds.y1;

	current_pi->step_p = step_p;
	current_pi->step_c = step_c;
	current_pi->step_r = step_r;
	current_pi->step_l = step_l;

	/* allocation for components and number of components has already been calculated by pi_create */
	for (uint32_t compno = 0; compno < current_pi->numcomps; ++compno) {
		auto current_comp = current_pi->comps + compno;
		auto img_comp = image->comps + compno;
		encoding_value_ptr = tmp_ptr[compno];

		current_comp->dx = img_comp->dx;
		current_comp->dy = img_comp->dy;
		/* resolutions have already been initialized */
		for (uint32_t resno = 0; resno < current_comp->numresolutions;	resno++) {
			auto res = current_comp->resolutions + resno;

			res->pdx = *(encoding_value_ptr++);
			res->pdy = *(encoding_value_ptr++);
			res->pw = *(encoding_value_ptr++);
			res->ph = *(encoding_value_ptr++);
		}
	}
	for (uint32_t pino = 1; pino < bound; ++pino) {
		current_pi = pi + pino;
		current_pi->tx0 = tileBounds.x0;
		current_pi->ty0 = tileBounds.y0;
		current_pi->tx1 = tileBounds.x1;
		current_pi->ty1 = tileBounds.y1;
		current_pi->step_p = step_p;
		current_pi->step_c = step_c;
		current_pi->step_r = step_r;
		current_pi->step_l = step_l;

		/* allocation for components and number of components has already been calculated by pi_create */
		for (uint32_t compno = 0; compno < current_pi->numcomps; ++compno) {
			auto current_comp = current_pi->comps + compno;
			auto img_comp = image->comps + compno;

			encoding_value_ptr = tmp_ptr[compno];
			current_comp->dx = img_comp->dx;
			current_comp->dy = img_comp->dy;
			/* resolutions have already been initialized */
			for (uint32_t resno = 0; resno < current_comp->numresolutions;	resno++) {
				auto res = current_comp->resolutions + resno;

				res->pdx = *(encoding_value_ptr++);
				res->pdy = *(encoding_value_ptr++);
				res->pw = *(encoding_value_ptr++);
				res->ph = *(encoding_value_ptr++);
			}
		}
	}
	grk_free(tmp_data);
	grk_free(tmp_ptr);

	bool poc = tcp->POC;

	for (uint32_t pino = 0; pino <= tcp->numpocs; ++pino) {
		auto piter = pi + pino;
		auto current_poc 		= tcp->pocs + pino;
		piter->poc.prg 	= poc ? current_poc->prg : tcp->prg; /* Progression Order #0 */
		piter->poc.layS 	= 0;
		piter->poc.layE 	= poc ? std::min<uint16_t>(current_poc->layE,
				tcp->numlayers) : tcp->numlayers; /* Layer Index #0 (End) */
		piter->poc.resS 	= poc ? current_poc->resS : 0; /* Resolution Level Index #0 (Start) */
		piter->poc.resE 	= poc ? current_poc->resE : max_res; /* Resolution Level Index #0 (End) */
		piter->poc.compS 	= poc ? current_poc->compS : 0; /* Component Index #0 (Start) */
		piter->poc.compE 	= poc ? current_poc->compE : piter->numcomps; /* Component Index #0 (End) */
		piter->poc.precS 	= 0;
		piter->poc.precE = max_precincts;

		piter->layno = piter->poc.layS;
		piter->resno = piter->poc.resS;
		piter->compno = piter->poc.compS;
		piter->precinctIndex = piter->poc.precS;

	}

	return pi;
}

PacketIter* pi_create_compress(const grk_image *image,
								CodingParams *p_cp,
								uint16_t tile_no,
								J2K_T2_MODE p_t2_mode,
								IncludeTracker *include) {
	assert(p_cp != nullptr);
	assert(image != nullptr);
	assert(tile_no < p_cp->t_grid_width * p_cp->t_grid_height);

	auto tcp = &p_cp->tcps[tile_no];
	uint32_t bound = tcp->numpocs + 1;
	uint32_t data_stride = 4 * GRK_J2K_MAXRLVLS;
	auto tmp_data = (uint32_t*) grk_malloc((size_t)data_stride * image->numcomps * sizeof(uint32_t));
	if (!tmp_data) {
		return nullptr;
	}

	auto tmp_ptr = (uint32_t**) grk_malloc(image->numcomps * sizeof(uint32_t*));
	if (!tmp_ptr) {
		grk_free(tmp_data);
		return nullptr;
	}
	auto pi = pi_create(image, p_cp, tile_no,include);
	if (!pi) {
		grk_free(tmp_data);
		grk_free(tmp_ptr);
		return nullptr;
	}

	auto encoding_value_ptr = tmp_data;
	for (uint32_t compno = 0; compno < image->numcomps; ++compno) {
		tmp_ptr[compno] = encoding_value_ptr;
		encoding_value_ptr += data_stride;
	}

	uint8_t max_res;
	uint64_t max_precincts;
	grk_rect_u32 tileBounds;
	uint32_t dx_min, dy_min;
	pi_get_encoding_params(image,
							p_cp,
							tile_no,
							&tileBounds,
							&dx_min,
							&dy_min,
							include->precincts,
							&max_precincts,
							&max_res,
							tmp_ptr);

	uint32_t step_p = 1;
	uint64_t step_c = max_precincts * step_p;
	uint64_t step_r = image->numcomps * step_c;
	uint64_t step_l = max_res * step_r;

	/* set values for first packet iterator*/
	pi->tp_on = p_cp->m_coding_params.m_enc.m_tp_on;
	auto piter = pi;

	/* special treatment for the first packet iterator*/
	piter->tx0 = tileBounds.x0;
	piter->ty0 = tileBounds.y0;
	piter->tx1 = tileBounds.x1;
	piter->ty1 = tileBounds.y1;
	piter->dx = dx_min;
	piter->dy = dy_min;
	piter->step_p = step_p;
	piter->step_c = step_c;
	piter->step_r = step_r;
	piter->step_l = step_l;

	/* allocation for components and number of components has already been calculated by pi_create */
	for (uint32_t compno = 0; compno < piter->numcomps; ++compno) {
		encoding_value_ptr = tmp_ptr[compno];

		auto current_comp = piter->comps + compno;
		auto img_comp = image->comps + compno;

		current_comp->dx = img_comp->dx;
		current_comp->dy = img_comp->dy;

		/* resolutions have already been initialized */
		for (uint32_t resno = 0; resno < current_comp->numresolutions;resno++) {
			auto res = current_comp->resolutions + resno;

			res->pdx = *(encoding_value_ptr++);
			res->pdy = *(encoding_value_ptr++);
			res->pw = *(encoding_value_ptr++);
			res->ph = *(encoding_value_ptr++);
		}
	}
	for (uint32_t pino = 1; pino < bound; ++pino) {
		piter = pi + pino;

		piter->tx0 = tileBounds.x0;
		piter->ty0 = tileBounds.y0;
		piter->tx1 = tileBounds.x1;
		piter->ty1 = tileBounds.y1;
		piter->dx = dx_min;
		piter->dy = dy_min;
		piter->step_p = step_p;
		piter->step_c = step_c;
		piter->step_r = step_r;
		piter->step_l = step_l;

		/* allocation for components and number of components
		 *  has already been calculated by pi_create */
		for (uint32_t compno = 0; compno < piter->numcomps; ++compno) {
			auto current_comp = piter->comps + compno;
			auto img_comp = image->comps + compno;

			encoding_value_ptr = tmp_ptr[compno];
			current_comp->dx = img_comp->dx;
			current_comp->dy = img_comp->dy;
			/* resolutions have already been initialized */
			for (uint32_t resno = 0; resno < current_comp->numresolutions; resno++) {
				auto res = current_comp->resolutions + resno;

				res->pdx = *(encoding_value_ptr++);
				res->pdy = *(encoding_value_ptr++);
				res->pw = *(encoding_value_ptr++);
				res->ph = *(encoding_value_ptr++);
			}
		}

		piter->layno = piter->poc.layS;
		piter->resno = piter->poc.resS;
		piter->compno = piter->poc.compS;
		piter->precinctIndex = piter->poc.precS;
	}
	grk_free(tmp_data);
	tmp_data = nullptr;
	grk_free(tmp_ptr);
	tmp_ptr = nullptr;

	bool poc = tcp->POC && (GRK_IS_CINEMA(p_cp->rsiz) || p_t2_mode == FINAL_PASS);
	pi_update_compress(p_cp, image->numcomps, tile_no, tileBounds, max_precincts, max_res, dx_min, dy_min, poc);

	return pi;
}

void pi_enable_tile_part_generation(PacketIter *pi,
									CodingParams *cp,
									uint16_t tileno,
									uint32_t pino,
									bool first_poc_tile_part,
									uint32_t tppos,
									J2K_T2_MODE t2_mode) {
	auto tcps = cp->tcps + tileno;
	auto poc = tcps->pocs + pino;
	auto prog = j2k_convert_progression_order(poc->prg);

	pi[pino].poc.prg = poc->prg;

	if (!(cp->m_coding_params.m_enc.m_tp_on
			&& ((!GRK_IS_CINEMA(cp->rsiz) && !GRK_IS_IMF(cp->rsiz)	&& t2_mode == FINAL_PASS) || GRK_IS_CINEMA(cp->rsiz)|| GRK_IS_IMF(cp->rsiz)))) {
		pi[pino].poc.layS = 0;
		pi[pino].poc.layE = poc->tpLayE;
		pi[pino].poc.resS = poc->tpResS;
		pi[pino].poc.resE = poc->tpResE;
		pi[pino].poc.compS = poc->tpCompS;
		pi[pino].poc.compE = poc->tpCompE;
		pi[pino].poc.precS = 0;
		pi[pino].poc.precE = poc->tpPrecE;
		pi[pino].poc.tx0 = poc->tp_txS;
		pi[pino].poc.ty0 = poc->tp_tyS;
		pi[pino].poc.tx1 = poc->tp_txE;
		pi[pino].poc.ty1 = poc->tp_tyE;
	} else {
		for (uint32_t i = tppos + 1; i < 4; i++) {
			switch (prog[i]) {
			case 'R':
				pi[pino].poc.resS = poc->tpResS;
				pi[pino].poc.resE = poc->tpResE;
				break;
			case 'C':
				pi[pino].poc.compS = poc->tpCompS;
				pi[pino].poc.compE = poc->tpCompE;
				break;
			case 'L':
				pi[pino].poc.layS = 0;
				pi[pino].poc.layE = poc->tpLayE;
				break;
			case 'P':
				switch (poc->prg) {
				case GRK_LRCP:
				case GRK_RLCP:
					pi[pino].poc.precS = 0;
					pi[pino].poc.precE = poc->tpPrecE;
					break;
				default:
					pi[pino].poc.tx0 = poc->tp_txS;
					pi[pino].poc.ty0 = poc->tp_tyS;
					pi[pino].poc.tx1 = poc->tp_txE;
					pi[pino].poc.ty1 = poc->tp_tyE;
					break;
				}
				break;
			}
		}

		if (first_poc_tile_part) {
			for (int32_t i = (int32_t) tppos; i >= 0; i--) {
				switch (prog[i]) {
				case 'C':
					poc->comp_temp = poc->tpCompS;
					pi[pino].poc.compS = poc->comp_temp;
					pi[pino].poc.compE = poc->comp_temp + 1;
					poc->comp_temp += 1;
					break;
				case 'R':
					poc->res_temp = poc->tpResS;
					pi[pino].poc.resS = poc->res_temp;
					pi[pino].poc.resE = poc->res_temp + 1;
					poc->res_temp += 1;
					break;
				case 'L':
					poc->lay_temp = 0;
					pi[pino].poc.layS = poc->lay_temp;
					pi[pino].poc.layE = poc->lay_temp + 1;
					poc->lay_temp += 1;
					break;
				case 'P':
					switch (poc->prg) {
					case GRK_LRCP:
					case GRK_RLCP:
						poc->prec_temp = 0;
						pi[pino].poc.precS = poc->prec_temp;
						pi[pino].poc.precE = poc->prec_temp + 1;
						poc->prec_temp += 1;
						break;
					default:
						poc->tx0_temp = poc->tp_txS;
						poc->ty0_temp = poc->tp_tyS;
						pi[pino].poc.tx0 = poc->tx0_temp;
						pi[pino].poc.tx1 = (poc->tx0_temp + poc->dx
								- (poc->tx0_temp % poc->dx));
						pi[pino].poc.ty0 = poc->ty0_temp;
						pi[pino].poc.ty1 = (poc->ty0_temp + poc->dy
								- (poc->ty0_temp % poc->dy));
						poc->tx0_temp = pi[pino].poc.tx1;
						poc->ty0_temp = pi[pino].poc.ty1;
						break;
					}
					break;
				}
			}
		} else {
			uint32_t incr_top = 1;
			uint32_t resetX = 0;
			for (int32_t i = (int32_t) tppos; i >= 0; i--) {
				switch (prog[i]) {
				case 'C':
					pi[pino].poc.compS = poc->comp_temp - 1;
					pi[pino].poc.compE = poc->comp_temp;
					break;
				case 'R':
					pi[pino].poc.resS = poc->res_temp - 1;
					pi[pino].poc.resE = poc->res_temp;
					break;
				case 'L':
					pi[pino].poc.layS = poc->lay_temp - 1;
					pi[pino].poc.layE = poc->lay_temp;
					break;
				case 'P':
					switch (poc->prg) {
					case GRK_LRCP:
					case GRK_RLCP:
						pi[pino].poc.precS = poc->prec_temp - 1;
						pi[pino].poc.precE = poc->prec_temp;
						break;
					default:
						pi[pino].poc.tx0 = (poc->tx0_temp - poc->dx
								- (poc->tx0_temp % poc->dx));
						pi[pino].poc.tx1 = poc->tx0_temp;
						pi[pino].poc.ty0 = (poc->ty0_temp - poc->dy
								- (poc->ty0_temp % poc->dy));
						pi[pino].poc.ty1 = poc->ty0_temp;
						break;
					}
					break;
				}
				if (incr_top == 1) {
					switch (prog[i]) {
					case 'R':
						if (poc->res_temp == poc->tpResE) {
							if (pi_check_next_for_valid_progression(i - 1, cp, tileno, pino,prog)) {
								poc->res_temp = poc->tpResS;
								pi[pino].poc.resS = poc->res_temp;
								pi[pino].poc.resE = poc->res_temp + 1;
								poc->res_temp += 1;
								incr_top = 1;
							} else {
								incr_top = 0;
							}
						} else {
							pi[pino].poc.resS = poc->res_temp;
							pi[pino].poc.resE = poc->res_temp + 1;
							poc->res_temp += 1;
							incr_top = 0;
						}
						break;
					case 'C':
						if (poc->comp_temp == poc->tpCompE) {
							if (pi_check_next_for_valid_progression(i - 1, cp, tileno, pino,prog)) {
								poc->comp_temp = poc->tpCompS;
								pi[pino].poc.compS = poc->comp_temp;
								pi[pino].poc.compE = poc->comp_temp + 1;
								poc->comp_temp += 1;
								incr_top = 1;
							} else {
								incr_top = 0;
							}
						} else {
							pi[pino].poc.compS = poc->comp_temp;
							pi[pino].poc.compE = poc->comp_temp + 1;
							poc->comp_temp += 1;
							incr_top = 0;
						}
						break;
					case 'L':
						if (poc->lay_temp == poc->tpLayE) {
							if (pi_check_next_for_valid_progression(i - 1, cp, tileno, pino,prog)) {
								poc->lay_temp = 0;
								pi[pino].poc.layS = poc->lay_temp;
								pi[pino].poc.layE = poc->lay_temp + 1;
								poc->lay_temp += 1;
								incr_top = 1;
							} else {
								incr_top = 0;
							}
						} else {
							pi[pino].poc.layS = poc->lay_temp;
							pi[pino].poc.layE = poc->lay_temp + 1;
							poc->lay_temp += 1;
							incr_top = 0;
						}
						break;
					case 'P':
						switch (poc->prg) {
						case GRK_LRCP:
						case GRK_RLCP:
							if (poc->prec_temp == poc->tpPrecE) {
								if (pi_check_next_for_valid_progression(i - 1, cp, tileno, pino,prog)) {
									poc->prec_temp = 0;
									pi[pino].poc.precS = poc->prec_temp;
									pi[pino].poc.precE = poc->prec_temp + 1;
									poc->prec_temp += 1;
									incr_top = 1;
								} else {
									incr_top = 0;
								}
							} else {
								pi[pino].poc.precS = poc->prec_temp;
								pi[pino].poc.precE = poc->prec_temp + 1;
								poc->prec_temp += 1;
								incr_top = 0;
							}
							break;
						default:
							if (poc->tx0_temp >= poc->tp_txE) {
								if (poc->ty0_temp >= poc->tp_tyE) {
									if (pi_check_next_for_valid_progression(i - 1, cp, tileno,pino, prog)) {
										poc->ty0_temp = poc->tp_tyS;
										pi[pino].poc.ty0 = poc->ty0_temp;
										pi[pino].poc.ty1 =
												(uint32_t) (poc->ty0_temp + poc->dy - (poc->ty0_temp % poc->dy));
										poc->ty0_temp = pi[pino].poc.ty1;
										incr_top = 1;
										resetX = 1;
									} else {
										incr_top = 0;
										resetX = 0;
									}
								} else {
									pi[pino].poc.ty0 = poc->ty0_temp;
									pi[pino].poc.ty1 = (poc->ty0_temp + poc->dy - (poc->ty0_temp % poc->dy));
									poc->ty0_temp = pi[pino].poc.ty1;
									incr_top = 0;
									resetX = 1;
								}
								if (resetX == 1) {
									poc->tx0_temp = poc->tp_txS;
									pi[pino].poc.tx0 = poc->tx0_temp;
									pi[pino].poc.tx1 = (uint32_t) (poc->tx0_temp + poc->dx - (poc->tx0_temp % poc->dx));
									poc->tx0_temp = pi[pino].poc.tx1;
								}
							} else {
								pi[pino].poc.tx0 = poc->tx0_temp;
								pi[pino].poc.tx1 = (uint32_t) (poc->tx0_temp + poc->dx - (poc->tx0_temp % poc->dx));
								poc->tx0_temp = pi[pino].poc.tx1;
								incr_top = 0;
							}
							break;
						}
						break;
					}
				}
			}
		}
	}
}

void pi_destroy(PacketIter *p_pi) {
	if (p_pi) {
		p_pi->destroy_include();
		delete[] p_pi;
	}
}

void pi_update_encoding_parameters(const grk_image *image,
									CodingParams *p_cp,
									uint16_t tileno) {
	assert(p_cp != nullptr);
	assert(image != nullptr);
	assert(tileno < p_cp->t_grid_width * p_cp->t_grid_height);

	auto tcp = p_cp->tcps + tileno;
	uint8_t max_res = 0;
	uint64_t max_precincts = 0;
	uint32_t dx_min= UINT_MAX, dy_min= UINT_MAX;

	/* position in x and y of tile */
	uint32_t tile_x = tileno % p_cp->t_grid_width;
	uint32_t tile_y = tileno / p_cp->t_grid_width;

	grk_rect_u32 tileBounds = p_cp->getTileBounds(image,tile_x,tile_y);
	for (uint32_t compno = 0; compno < image->numcomps; ++compno) {
		auto comp = image->comps + compno;
		auto tccp = tcp->tccps + compno;
		auto tileCompBounds = tileBounds.rectceildiv(comp->dx, comp->dy);

		if (tccp->numresolutions > max_res)
			max_res = tccp->numresolutions;
		/* use custom size for precincts */
		for (uint32_t resno = 0; resno < tccp->numresolutions; ++resno) {
			/* precinct width and height */
			uint32_t pdx = tccp->prcw_exp[resno];
			uint32_t pdy = tccp->prch_exp[resno];

			uint64_t dx = 	comp->dx* ((uint64_t) 1u << (pdx + tccp->numresolutions - 1 - resno));
			uint64_t dy = 	comp->dy* ((uint64_t) 1u << (pdy + tccp->numresolutions - 1 - resno));

			/* take the minimum size for dx for each comp and resolution */
			if (dx < UINT_MAX)
				dx_min = std::min<uint32_t>(dx_min, (uint32_t) dx);
			if (dy < UINT_MAX)
				dy_min = std::min<uint32_t>(dy_min, (uint32_t) dy);

			uint32_t level_no = tccp->numresolutions - 1 - resno;
			auto resBounds = tileCompBounds.rectceildivpow2(level_no);
			uint32_t px0 = uint_floordivpow2(resBounds.x0, pdx) << pdx;
			uint32_t py0 = uint_floordivpow2(resBounds.y0, pdy) << pdy;
			uint32_t px1 = ceildivpow2<uint32_t>(resBounds.x1, pdx) << pdx;
			uint32_t py1 = ceildivpow2<uint32_t>(resBounds.y1, pdy) << pdy;
			uint32_t pw = (resBounds.width() == 0) ? 0 : ((px1 - px0) >> pdx);
			uint32_t ph = (resBounds.height() ==0 ) ? 0 : ((py1 - py0) >> pdy);
			uint64_t product = (uint64_t)pw * ph;
			if (product > max_precincts)
				max_precincts = product;
		}
	}
	pi_update_compress(p_cp,
						image->numcomps,
						tileno,
						tileBounds,
						max_precincts,
						max_res,
						dx_min,
						dy_min,
						tcp->POC);
}



PacketIter::PacketIter() : tp_on(false),
							includeTracker(nullptr),
							step_l(0),
							step_r(0),
							step_c(0),
							step_p(0),
							compno(0),
							resno(0),
							precinctIndex(0),
							layno(0),
							first(true),
							numcomps(0),
							comps(nullptr),
							tx0(0),
							ty0(0),
							tx1(0),
							ty1(0),
							x(0),
							y(0),
							dx(0),
							dy(0)
{
	memset(&poc, 0, sizeof(poc));
}

PacketIter::~PacketIter(){
	if (comps) {
		for (uint32_t compno = 0; compno < numcomps; compno++)
			grk_free((comps + compno)->resolutions);
		grk_free(comps);
	}
}

bool PacketIter::next_cprl(void) {
	grk_pi_comp *comp = nullptr;
	if (compno >= numcomps){
		GRK_ERROR("Packet iterator component %d must be strictly less than "
				"total number of components %d",compno , numcomps);
		return false;
	}

	for (compno = poc.compS; compno < poc.compE; compno++) {
		comp = &comps[compno];
		dx = 0;
		dy = 0;
		update_dxy_for_comp(comp);
		if (!tp_on) {
			poc.ty0 = ty0;
			poc.tx0 = tx0;
			poc.ty1 = ty1;
			poc.tx1 = tx1;
		}
		for (y = poc.ty0; y < poc.ty1;	y += dy - (y % dy)) {
			for (x = poc.tx0; x < poc.tx1;	x += dx - (x % dx)) {
				for (resno = poc.resS; resno < std::min<uint32_t>(poc.resE, comp->numresolutions); resno++) {
					if (!next_l())
						continue;
					for (layno = poc.layS; layno < poc.layE; layno++) {
						if (update_include())
							return true;
					}
				}
			}
		}
	}

	return false;
}

bool PacketIter::next_l(void){
	if (compno >= numcomps){
		GRK_ERROR("Packet iterator component %d must be strictly less than "
				"total number of components %d",compno , numcomps);
		return false;
	}
	auto comp = comps + compno;
	if (resno >= comp->numresolutions)
		return false;

	auto res = comp->resolutions + resno;
	uint32_t levelno = comp->numresolutions - 1 - resno;
	if (levelno >= GRK_J2K_MAXRLVLS)
		return false;
	uint32_t trx0 = ceildiv<uint64_t>((uint64_t) tx0,
			((uint64_t) comp->dx << levelno));
	uint32_t try0 = ceildiv<uint64_t>((uint64_t) ty0,
			((uint64_t) comp->dy << levelno));
	uint32_t trx1 = ceildiv<uint64_t>((uint64_t) tx1,
			((uint64_t) comp->dx << levelno));
	uint32_t try1 = ceildiv<uint64_t>((uint64_t) ty1,
			((uint64_t) comp->dy << levelno));
	uint32_t rpx = res->pdx + levelno;
	uint32_t rpy = res->pdy + levelno;
	if (!(((uint64_t) y % ((uint64_t) comp->dy << rpy) == 0)
			|| ((y == ty0)	&& (((uint64_t) try0 << levelno) % ((uint64_t) 1 << rpy))))) {
		return false;
	}
	if (!(((uint64_t) x % ((uint64_t) comp->dx << rpx) == 0)
			|| ((x == tx0)	&& (((uint64_t) trx0 << levelno) % ((uint64_t) 1 << rpx))))) {
		return false;
	}

	if ((res->pw == 0) || (res->ph == 0))
		return false;

	if ((trx0 == trx1) || (try0 == try1))
		return false;

	uint32_t prci = uint_floordivpow2(ceildiv<uint64_t>((uint64_t) x,	((uint64_t) comp->dx << levelno)), res->pdx)
			- uint_floordivpow2(trx0, res->pdx);
	uint32_t prcj = uint_floordivpow2(	ceildiv<uint64_t>((uint64_t) y,	((uint64_t) comp->dy << levelno)), res->pdy)
			- uint_floordivpow2(try0, res->pdy);
	precinctIndex = (prci + (uint64_t)prcj * res->pw);
	//skip precinct numbers greater than total number of precincts
	// for this resolution
	return (precinctIndex < (uint64_t)res->pw * res->ph);
}


bool PacketIter::next_pcrl(void) {
	grk_pi_comp *comp = nullptr;

	if (compno >= numcomps){
		GRK_ERROR("Packet iterator component %d must be strictly less than "
				"total number of components %d",compno , numcomps);
		return false;
	}
	update_dxy();
	if (!tp_on) {
		poc.ty0 = ty0;
		poc.tx0 = tx0;
		poc.ty1 = ty1;
		poc.tx1 = tx1;
	}
	for (y = poc.ty0; y < poc.ty1;	y += dy - (y % dy)) {
		for (x = poc.tx0; x < poc.tx1;	x += dx - (x % dx)) {
			for (compno = poc.compS; compno < poc.compE; compno++) {
				comp = &comps[compno];
				for (resno = poc.resS; resno
								< std::min<uint32_t>(poc.resE,comp->numresolutions); resno++) {
					if (!next_l())
						continue;
					for (layno = poc.layS; layno < poc.layE; layno++) {
						if (update_include())
							return true;
					}
				}
			}
		}
	}

	return false;
}

bool PacketIter::next_lrcp(void) {
	grk_pi_comp *comp = nullptr;
	grk_pi_resolution *res = nullptr;
	uint64_t precE;
/*
 	for (; layno < poc.layE; layno++) {
		for (; resno < poc.resE;resno++) {
			for (; compno < poc.compE;compno++) {
				comp = comps + compno;
				//skip resolutions greater than current component resolution
				if (resno >= comp->numresolutions)
					continue;
				res = comp->resolutions + resno;
				precE = (uint64_t)res->pw * res->ph;
				if (tp_on)
					precE = std::min<uint64_t>(precE, poc.precE);
				if (first){
					first = false;
					return true;
				}
				if (++precinctIndex < precE)
					return true;
				precinctIndex = poc.precS;
				first = true;
			}
			compno = poc.compS;
		}
		resno = poc.resS;
	}
 */
	for (layno = poc.layS; layno < poc.layE; layno++) {
		for (resno = poc.resS; resno < poc.resE;resno++) {
			for (compno = poc.compS; compno < poc.compE;compno++) {
				comp = comps + compno;
				//skip resolutions greater than current component resolution
				if (resno >= comp->numresolutions)
					continue;
				res = comp->resolutions + resno;
				precE = (uint64_t)res->pw * res->ph;
				if (tp_on)
					precE = std::min<uint64_t>(precE, poc.precE);
				for (precinctIndex = poc.precS; precinctIndex < precE;	precinctIndex++) {
					if (update_include())
						return true;
				}
			}
		}
	}

	return false;
}

bool PacketIter::next_rlcp(void) {
	grk_pi_comp *comp = nullptr;
	grk_pi_resolution *res = nullptr;
	uint64_t precE;

	if (compno >= numcomps){
		GRK_ERROR("Packet iterator component %d must be strictly less than "
				"total number of components %d",compno , numcomps);
		return false;
	}
	for (resno = poc.resS; resno < poc.resE; resno++) {
		for (layno = poc.layS; layno < poc.layE;	layno++) {
			for (compno = poc.compS; compno < poc.compE;compno++) {
				comp = comps + compno;
				if (resno >= comp->numresolutions)
					continue;
				res = comp->resolutions + resno;
				precE = (uint64_t)res->pw * res->ph;
				if (tp_on)
					precE = std::min<uint64_t>(precE, poc.precE);
				for (precinctIndex = poc.precS; precinctIndex < precE;	precinctIndex++) {
					if (update_include())
						return true;
				}
			}
		}
	}
	return false;
}

bool PacketIter::next_rpcl(void) {
	update_dxy();
	if (!tp_on) {
		poc.ty0 = ty0;
		poc.tx0 = tx0;
		poc.ty1 = ty1;
		poc.tx1 = tx1;
	}
	for (resno = poc.resS; resno < poc.resE; resno++) {
		for (y = poc.ty0; y < poc.ty1;	y += dy - (y % dy)) {
			for (x = poc.tx0; x < poc.tx1;	x += dx - (x % dx)) {
				for (compno = poc.compS; compno < poc.compE; compno++) {
					if (!next_l())
						continue;
					for (layno = poc.layS; layno < poc.layE; layno++) {
						if (update_include())
							return true;
					}
				}
			}
		}
	}

	return false;
}

bool PacketIter::next(void) {
	switch (poc.prg) {
		case GRK_LRCP:
			return next_lrcp();
		case GRK_RLCP:
			return next_rlcp();
		case GRK_RPCL:
			return next_rpcl();
		case GRK_PCRL:
			return next_pcrl();
		case GRK_CPRL:
			return next_cprl();
		default:
			return false;
	}

	return false;
}

void PacketIter::update_dxy(void) {
	dx = 0;
	dy = 0;
	for (uint32_t compno = 0; compno < numcomps; compno++)
		update_dxy_for_comp(comps + compno);
}

void PacketIter::update_dxy_for_comp(grk_pi_comp *comp) {
	for (uint32_t resno = 0; resno < comp->numresolutions; resno++) {
		auto res = comp->resolutions + resno;
		uint64_t dx_temp = comp->dx	* ((uint64_t) 1u << (res->pdx + comp->numresolutions - 1 - resno));
		uint64_t dy_temp = comp->dy	* ((uint64_t) 1u << (res->pdy + comp->numresolutions - 1 - resno));
		if (dx_temp < UINT_MAX)
			dx = !dx ?	(uint32_t) dx_temp : std::min<uint32_t>(dx, (uint32_t) dx_temp);
		if (dy_temp < UINT_MAX)
			dy = !dy ?	(uint32_t) dy_temp : std::min<uint32_t>(dy, (uint32_t) dy_temp);
	}
}

uint8_t* PacketIter::get_include(uint16_t layerno){
	return includeTracker->get_include(layerno, resno);
}

bool PacketIter::update_include(void){
	return includeTracker->update(layno, resno, compno, precinctIndex);
}

void PacketIter::destroy_include(void){
	includeTracker->clear();
}

}
