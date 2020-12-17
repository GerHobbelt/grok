/*
 *    Copyright (C) 2016-2020 Grok Image Compression Inc.
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
 */

#include "grk_includes.h"

namespace grk {


Segment::Segment() {
	clear();
}
void Segment::clear() {
	dataindex = 0;
	numpasses = 0;
	len = 0;
	maxpasses = 0;
	numPassesInPacket = 0;
	numBytesInPacket = 0;
}

PacketLengthInfo::PacketLengthInfo(uint32_t mylength, uint32_t bits) :
		len(mylength), len_bits(bits) {
}
PacketLengthInfo::PacketLengthInfo() :
		len(0), len_bits(0) {
}
bool PacketLengthInfo::operator==(const PacketLengthInfo &rhs) const {
	return (rhs.len == len && rhs.len_bits == len_bits);
}

CodePass::CodePass() :
		rate(0), distortiondec(0), len(0), term(0), slope(0) {
}
Layer::Layer() :
		numpasses(0), len(0), disto(0), data(nullptr) {
}

Precinct::Precinct() :
		precinctIndex(0),
		impl(nullptr),
		initialized(false)
{
}
Precinct::~Precinct(){
	delete impl;
}

bool Precinct::init(bool isCompressor,
					grk_pt cblk_expn,
					grk_plugin_tile *current_plugin_tile){
	if (initialized)
		return true;
	impl = new PrecinctImpl();
	initialized =  impl->init(isCompressor,this,cblk_expn,current_plugin_tile);

	return initialized;

}

void Precinct::deleteTagTrees() {
	if (impl)
		impl->deleteTagTrees();
}

void Precinct::initTagTrees() {
	if (impl)
		impl->initTagTrees();
}

uint32_t Precinct::getCblkGridwidth(void){
	return impl ? impl->cblk_grid_width : 0;
}
uint32_t Precinct::getCblkGridHeight(void){
	return impl ? impl->cblk_grid_height : 0;
}
uint64_t Precinct::getNumCblks(void){
	return impl ? (uint64_t)impl->cblk_grid_width * impl->cblk_grid_height : 0;
}
CompressCodeblock* Precinct::getCompressedBlockPtr(void){
	return impl ? impl->enc : nullptr;
}
DecompressCodeblock* Precinct::getDecompressedBlockPtr(void){
	return impl ? impl->dec : nullptr;
}
TagTree* Precinct::getInclTree(void){
	return impl ? impl->incltree : nullptr;
}
TagTree* Precinct::getImsbTree(void){
	return impl ? impl->imsbtree : nullptr;
}

PrecinctImpl::PrecinctImpl() :
		cblk_grid_width(0), cblk_grid_height(0),
		enc(nullptr), dec(nullptr),
		incltree(nullptr), imsbtree(nullptr) {
}
PrecinctImpl::~PrecinctImpl(){
	deleteTagTrees();
	delete[] enc;
	delete[] dec;
}

bool PrecinctImpl::init(bool isCompressor,
					grk_rect_u32 *bounds,
					grk_pt cblk_expn,
					grk_plugin_tile *current_plugin_tile){


	auto cblk_grid = grk_rect_u32(
			uint_floordivpow2(bounds->x0,cblk_expn.x),
			uint_floordivpow2(bounds->y0,cblk_expn.y),
			ceildivpow2<uint32_t>(bounds->x1,cblk_expn.x),
			ceildivpow2<uint32_t>(bounds->y1,cblk_expn.y));


	uint32_t state = grk_plugin_get_debug_state();
	size_t nominalBlockSize = (1 << cblk_expn.x) * (1 << cblk_expn.y);
	cblk_grid_width 	= cblk_grid.width();
	cblk_grid_height 	= cblk_grid.height();


	uint64_t nb_code_blocks = cblk_grid.area();
	if (!nb_code_blocks)
		return true;

	if (isCompressor)
		enc = new CompressCodeblock[nb_code_blocks];
	else
		dec = new DecompressCodeblock[nb_code_blocks];
	initTagTrees();

	for (uint64_t cblkno = 0; cblkno < nb_code_blocks; ++cblkno) {
		auto cblk_start = grk_pt(	(cblk_grid.x0  + (uint32_t) (cblkno % cblk_grid_width)) << cblk_expn.x,
									(cblk_grid.y0  + (uint32_t) (cblkno / cblk_grid_width)) << cblk_expn.y);
		auto cblk_bounds = grk_rect_u32(cblk_start.x,
										cblk_start.y,
										cblk_start.x + (1 << cblk_expn.x),
										cblk_start.y + (1 << cblk_expn.y));

		auto cblk_dims = (isCompressor) ?
									(grk_rect_u32*)(enc + cblkno) :
									(grk_rect_u32*)(dec + cblkno);
		if (isCompressor) {
			auto code_block = enc + cblkno;
			if (!code_block->alloc())
				return false;
			if (!current_plugin_tile
					|| (state & GRK_PLUGIN_STATE_DEBUG)) {
				if (!code_block->alloc_data(nominalBlockSize))
					return false;
			}
		} else {
			auto code_block = dec + cblkno;
			if (!current_plugin_tile
					|| (state & GRK_PLUGIN_STATE_DEBUG)) {
				if (!code_block->alloc())
					return false;
			}
		}
		*cblk_dims = cblk_bounds.intersection(bounds);
	}

	return true;
}

void PrecinctImpl::deleteTagTrees() {
	delete incltree;
	incltree = nullptr;
	delete imsbtree;
	imsbtree = nullptr;
}

void PrecinctImpl::initTagTrees() {

	// if cw == 0 or ch == 0,
	// then the precinct has no code blocks, therefore
	// no need for inclusion and msb tag trees
	if (cblk_grid_width > 0 && cblk_grid_height > 0) {
		if (!incltree) {
			try {
				incltree = new TagTree(cblk_grid_width, cblk_grid_height);
			} catch (std::exception &e) {
				GRK_WARN("No incltree created.");
			}
		} else {
			if (!incltree->init(cblk_grid_width, cblk_grid_height)) {
				GRK_WARN("Failed to re-initialize incltree.");
				delete incltree;
				incltree = nullptr;
			}
		}

		if (!imsbtree) {
			try {
				imsbtree = new TagTree(cblk_grid_width, cblk_grid_height);
			} catch (std::exception &e) {
				GRK_WARN("No imsbtree created.");
			}
		} else {
			if (!imsbtree->init(cblk_grid_width, cblk_grid_height)) {
				GRK_WARN("Failed to re-initialize imsbtree.");
				delete imsbtree;
				imsbtree = nullptr;
			}
		}
	}
}


Codeblock::Codeblock():
		numbps(0),
		numlenbits(0),
		numPassesInPacket(0)
#ifdef DEBUG_LOSSLESS_T2
		,included(false),
#endif
{
}

Codeblock::Codeblock(const Codeblock &rhs): grk_rect_u32(rhs),
											compressedStream(rhs.compressedStream),
											numbps(rhs.numbps),
											numlenbits(rhs.numlenbits),
											numPassesInPacket(rhs.numPassesInPacket)
#ifdef DEBUG_LOSSLESS_T2
	,included(0)
#endif
{
}
Codeblock& Codeblock::operator=(const Codeblock& rhs){
	if (this != &rhs) { // self-assignment check expected
		x0 = rhs.x0;
		y0 = rhs.y0;
		x1 = rhs.x1;
		y1 = rhs.y1;
		compressedStream = rhs.compressedStream;
		numbps = rhs.numbps;
		numlenbits = rhs.numlenbits;
		numPassesInPacket = rhs.numPassesInPacket;
#ifdef DEBUG_LOSSLESS_T2
		included = rhs.included;
		packet_length_info = rhs.packet_length_info;
#endif
	}
	return *this;
}
void Codeblock::clear(){
	compressedStream.buf = nullptr;
	compressedStream.owns_data = false;
}
CompressCodeblock::CompressCodeblock() :
				paddedCompressedStream(nullptr),
				layers(	nullptr),
				passes(nullptr),
				numPassesInPreviousPackets(0),
				numPassesTotal(0),
				contextStream(nullptr)
{
}
CompressCodeblock::CompressCodeblock(const CompressCodeblock &rhs) :
						Codeblock(rhs),
						paddedCompressedStream(rhs.paddedCompressedStream),
						layers(	rhs.layers),
						passes(rhs.passes),
						numPassesInPreviousPackets(rhs.numPassesInPreviousPackets),
						numPassesTotal(rhs.numPassesTotal),
						contextStream(rhs.contextStream)
{}

CompressCodeblock& CompressCodeblock::operator=(const CompressCodeblock& rhs){
	if (this != &rhs) { // self-assignment check expected
		Codeblock::operator = (rhs);
		paddedCompressedStream = rhs.paddedCompressedStream;
		layers = rhs.layers;
		passes = rhs.passes;
		numPassesInPreviousPackets = rhs.numPassesInPreviousPackets;
		numPassesTotal = rhs.numPassesTotal;
		contextStream = rhs.contextStream;
#ifdef DEBUG_LOSSLESS_T2
		packet_length_info = rhs.packet_length_info;
#endif

	}
	return *this;
}

CompressCodeblock::~CompressCodeblock() {
	cleanup();
}
void CompressCodeblock::clear(){
	Codeblock::clear();
	layers = nullptr;
	passes = nullptr;
	contextStream = nullptr;
#ifdef DEBUG_LOSSLESS_T2
	packet_length_info.clear();
#endif
}
bool CompressCodeblock::alloc() {
	if (!layers) {
		/* no memset since data */
		layers = (Layer*) grk_calloc(100, sizeof(Layer));
		if (!layers)
			return false;
	}
	if (!passes) {
		passes = (CodePass*) grk_calloc(100, sizeof(CodePass));
		if (!passes)
			return false;
	}

	return true;
}

/**
 * Allocates data memory for an compressing code block.
 * We actually allocate 2 more bytes than specified, and then offset data by +2.
 * This is done so that we can safely initialize the MQ coder pointer to data-1,
 * without risk of accessing uninitialized memory.
 */
bool CompressCodeblock::alloc_data(size_t nominalBlockSize) {
	uint32_t desired_data_size = (uint32_t) (nominalBlockSize * sizeof(uint32_t));
	// we add two fake zero bytes at beginning of buffer, so that mq coder
	//can be initialized to data[-1] == actualData[1], and still point
	//to a valid memory location
	auto buf =  new uint8_t[desired_data_size + grk_cblk_enc_compressed_data_pad_left];
	buf[0] = 0;
	buf[1] = 0;

	paddedCompressedStream = buf + grk_cblk_enc_compressed_data_pad_left;
	compressedStream.buf = buf;
	compressedStream.len = desired_data_size;
	compressedStream.owns_data = true;

	return true;
}

void CompressCodeblock::cleanup() {
	compressedStream.dealloc();
	paddedCompressedStream = nullptr;
	grk_free(layers);
	layers = nullptr;
	grk_free(passes);
	passes = nullptr;
}

DecompressCodeblock::DecompressCodeblock() {
	init();
}

DecompressCodeblock::~DecompressCodeblock(){
    cleanup();
}
void DecompressCodeblock::clear(){
	Codeblock::clear();
	segs = nullptr;
	cleanup_seg_buffers();
}

DecompressCodeblock::DecompressCodeblock(const DecompressCodeblock &rhs) :
				Codeblock(rhs),
				segs(rhs.segs), numSegments(rhs.numSegments),
				numSegmentsAllocated(rhs.numSegmentsAllocated)
{}

DecompressCodeblock& DecompressCodeblock::operator=(const DecompressCodeblock& rhs){
	if (this != &rhs) { // self-assignment check expected
		*this = DecompressCodeblock(rhs);
	}
	return *this;
}

bool DecompressCodeblock::alloc() {
	if (!segs) {
		segs = new Segment[default_numbers_segments];
		/*fprintf(stderr, "Allocate %u elements of code_block->data\n", default_numbers_segments * sizeof(Segment));*/

		numSegmentsAllocated = default_numbers_segments;

		/*fprintf(stderr, "Allocate 8192 elements of code_block->data\n");*/
		/*fprintf(stderr, "numSegmentsAllocated of code_block->data = %u\n", p_code_block->numSegmentsAllocated);*/

	} else {
		/* sanitize */
		auto l_segs = segs;
		uint32_t l_current_max_segs = numSegmentsAllocated;

		/* Note: since seg_buffers simply holds references to another data buffer,
		 we do not need to copy it to the sanitized block  */
		cleanup_seg_buffers();
		init();
		segs = l_segs;
		numSegmentsAllocated = l_current_max_segs;
	}
	return true;
}

void DecompressCodeblock::init() {
	compressedStream.dealloc();
	segs = nullptr;
	x0 = 0;
	y0 = 0;
	x1 = 0;
	y1 = 0;
	numbps = 0;
	numlenbits = 0;
	numPassesInPacket = 0;
	numSegments = 0;
#ifdef DEBUG_LOSSLESS_T2
	included = 0;
#endif
	numSegmentsAllocated = 0;
}

void DecompressCodeblock::cleanup() {
	compressedStream.dealloc();
	cleanup_seg_buffers();
	delete[] segs;
	segs = nullptr;
}

void DecompressCodeblock::cleanup_seg_buffers(){

	for (auto& b : seg_buffers)
		delete b;
	seg_buffers.clear();

}

size_t DecompressCodeblock::getSegBuffersLen(){
	return std::accumulate(seg_buffers.begin(), seg_buffers.end(), 0, [](const size_t s, grk_buf *a){
	   return s + a->len;
	});
}

bool DecompressCodeblock::copy_to_contiguous_buffer(uint8_t *buffer) {
	if (!buffer)
		return false;
	size_t offset = 0;
	for (auto& buf : seg_buffers){
		if (buf->len){
			memcpy(buffer + offset, buf->buf, buf->len);
			offset += buf->len;
		}
	}
	return true;
}

Subband::Subband() :
				orientation(BAND_ORIENT_LL),
				numPrecincts(0),
				numbps(0),
				stepsize(0),
				inv_step(0) {
}

//note: don't copy precinct array
Subband::Subband(const Subband &rhs) : grk_rect_u32(rhs),
										orientation(rhs.orientation),
										numPrecincts(0),
										numbps(rhs.numbps),
										stepsize(rhs.stepsize),
										inv_step(rhs.inv_step)
{
}

Subband& Subband::operator= (const Subband &rhs){
	if (this != &rhs) { // self-assignment check expected
		*this = Subband(rhs);
	}
	return *this;
}

void Subband::print(){
	grk_rect_u32::print();
}


bool Subband::isEmpty() {
	return ((x1 - x0 == 0) || (y1 - y0 == 0));
}

Precinct* Subband::getPrecinct(uint64_t precinctIndex){
	if (precinctMap.find(precinctIndex) == precinctMap.end())
		return nullptr;
	uint64_t index = precinctMap[precinctIndex];

	return precincts[index];
}

Precinct* Subband::createPrecinct(bool isCompressor,
					uint64_t precinctIndex,
					grk_pt precinct_start,
					grk_pt precinct_expn,
					uint32_t pw,
					grk_pt cblk_expn,
					grk_plugin_tile *current_plugin_tile){

	auto temp = precinctMap.find(precinctIndex);
	if (temp != precinctMap.end())
		return precincts[temp->second];

	auto band_precinct_start = grk_pt(	precinct_start.x + (uint32_t)((precinctIndex % pw) << precinct_expn.x),
			precinct_start.y + (uint32_t)((precinctIndex / pw) << precinct_expn.y));
	auto precinct_dim = grk_rect_u32(
			band_precinct_start.x,
			band_precinct_start.y,
			band_precinct_start.x + (1 << precinct_expn.x),
			band_precinct_start.y + (1 << precinct_expn.y)).intersection(this);

	auto current_precinct = new Precinct();
	current_precinct->set_rect(precinct_dim);

	if (isCompressor) {
		if (!current_precinct->init(isCompressor,cblk_expn,current_plugin_tile)){
			delete current_precinct;
			return nullptr;
		}
	}

	current_precinct->precinctIndex = precinctIndex;
	precincts.push_back(current_precinct);
	precinctMap[precinctIndex] = precincts.size()-1;

	return current_precinct;
}



Resolution::Resolution() :
		initialized(false),
		numBandWindows(0),
		pw(0),
		ph(0),
		current_plugin_tile(nullptr)
{}

void Resolution::print(){
	grk_rect_u32::print();
	for (uint32_t i = 0; i < numBandWindows; ++i){
		std::cout << "band " << i << " : ";
		band[i].print();
	}
}

bool Resolution::init(bool isCompressor,
			TileComponentCodingParams *tccp,
			uint8_t resno,
			grk_plugin_tile *current_plugin_tile){

	if (initialized)
		return true;

	this->current_plugin_tile = current_plugin_tile;

	/* p. 35, table A-23, ISO/IEC FDIS154444-1 : 2000 (18 august 2000) */
	precinct_expn = grk_pt(tccp->prcw[resno],tccp->prch[resno]);

	/* p. 64, B.6, ISO/IEC FDIS15444-1 : 2000 (18 august 2000)  */
	precinct_start = grk_pt(uint_floordivpow2(x0, precinct_expn.x) << precinct_expn.x,
							uint_floordivpow2(y0, precinct_expn.y) << precinct_expn.y);

	uint64_t num_precincts = (uint64_t)pw * ph;
	if (mult64_will_overflow(num_precincts, sizeof(Precinct))) {
		GRK_ERROR(	"nb_precinct_size calculation would overflow ");
		return false;
	}
	if (resno != 0) {
		precinct_start=  grk_pt(ceildivpow2<uint32_t>(precinct_start.x, 1),
								ceildivpow2<uint32_t>(precinct_start.y, 1));
		precinct_expn.x--;
		precinct_expn.y--;
	}
	cblk_expn    =  grk_pt(std::min<uint32_t>(tccp->cblkw, precinct_expn.x),
						   std::min<uint32_t>(tccp->cblkh, precinct_expn.y));
	for (uint8_t bandIndex = 0; bandIndex < numBandWindows; ++bandIndex) {
		auto curr_band = band + bandIndex;
		curr_band->numPrecincts = num_precincts;
		if (isCompressor) {
			for (uint64_t precinctIndex = 0; precinctIndex < num_precincts; ++precinctIndex) {
				if (!curr_band->createPrecinct(true,
									precinctIndex,
									precinct_start,
									precinct_expn,
									pw,
									cblk_expn,
									current_plugin_tile))
					return false;

			}
		}
	}
	initialized = true;

	return true;
}

BlockExec::BlockExec() : 	tilec(nullptr),
							bandIndex(0),
							band_orientation(BAND_ORIENT_LL),
							stepsize(0),
							cblk_sty(0),
							qmfbid(0),
							x(0),
							y(0),
							k_msbs(0),
							isOpen(false)
{}

CompressBlockExec::CompressBlockExec() :
		            cblk(nullptr),
					tile(nullptr),
					doRateControl(false),
					distortion(0),
					tiledp(nullptr),
					compno(0),
					resno(0),
					precinctIndex(0),
					cblkno(0),
					inv_step(0),
					inv_step_ht(0),
					mct_norms(nullptr),
#ifdef DEBUG_LOSSLESS_T1
	unencodedData(nullptr),
#endif
				mct_numcomps(0)
{}

bool CompressBlockExec::open(T1Interface *t1){
	isOpen = t1->compress(this);

	return isOpen;
}
void CompressBlockExec::close(void){

}

DecompressBlockExec::DecompressBlockExec() :
				cblk(nullptr),
				resno(0),
				roishift(0)
{	}

bool DecompressBlockExec::open(T1Interface *t1){
	isOpen =  t1->decompress(this);

	return isOpen;
}
void DecompressBlockExec::close(void){

}

}
