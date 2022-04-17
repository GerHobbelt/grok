#include "grk_includes.h"

namespace grk
{
static bool reclaimCallback(grk_serialize_buf buffer, void* serialize_user_data)
{
	auto pool = (StripCache*)serialize_user_data;
	if(pool)
		pool->returnBufferToPool(GrkSerializeBuf(buffer));

	return true;
}

Strip::Strip(GrkImage* outputImage, uint16_t index, uint32_t height, uint8_t reduce)
	: stripImg(nullptr), tileCounter(0), index_(index), reduce_(reduce)
{
	stripImg = new GrkImage();
	outputImage->copyHeader(stripImg);

	stripImg->y0 = outputImage->y0 + index * height;
	stripImg->y1 = std::min<uint32_t>(outputImage->y1, stripImg->y0 + height);
	stripImg->comps->y0 = reduceDim(stripImg->y0);
	stripImg->comps->h = reduceDim(stripImg->y1 - stripImg->y0);
}
Strip::~Strip(void)
{
	grk_object_unref(&stripImg->obj);
}
uint32_t Strip::getIndex(void)
{
	return index_;
}
uint32_t Strip::reduceDim(uint32_t dim)
{
	return reduce_ ? ceildivpow2<uint32_t>(dim, reduce_) : dim;
}
StripCache::StripCache()
	: strips(nullptr), numTilesWidth_(0), numStrips_(0), stripHeight_(0), imageY0_(0), packedRowBytes_(0),
	  serializeUserData_(nullptr), serializeBufferCallback_(nullptr)
{}
StripCache::~StripCache()
{
	for(auto& b : pool)
		b.second.dealloc();
	for(uint16_t i = 0; i < numStrips_; ++i)
		delete strips[i];
	delete[] strips;
}
void StripCache::init(uint16_t numTilesWidth, uint16_t numStrips, uint32_t stripHeight, uint8_t reduce,
					  GrkImage* outputImage, grk_serialize_pixels_callback serializeBufferCallback,
					  void* serializeUserData,
					  grk_serialize_register_client_callback serializeRegisterClientCallback)
{
	assert(outputImage);
	if(!numStrips || !outputImage)
		return;
	serializeBufferCallback_ = serializeBufferCallback;
	serializeUserData_ = serializeUserData;
	if(serializeRegisterClientCallback)
		serializeRegisterClientCallback(reclaimCallback, serializeUserData, this);
	numTilesWidth_ = numTilesWidth;
	numStrips_ = numStrips;
	imageY0_ = outputImage->y0;
	stripHeight_ = stripHeight;
	packedRowBytes_ = outputImage->packedRowBytes;
	strips = new Strip*[numStrips];
	for(uint16_t i = 0; i < numStrips_; ++i)
		strips[i] = new Strip(outputImage, i, stripHeight_, reduce);
}
bool StripCache::ingestTile(GrkImage* src)
{
	uint16_t stripId = (uint16_t)((src->y0 - imageY0_ + stripHeight_ - 1) / stripHeight_);
	assert(stripId < numStrips_);
	auto strip = strips[stripId];
	auto dest = strip->stripImg;
	// use height of first component, because no subsampling
	uint64_t dataLen = packedRowBytes_ * src->comps->h;
	{
		std::unique_lock<std::mutex> lk(poolMutex_);
		if(!dest->interleavedData.data)
		{
			dest->interleavedData = getBufferFromPool(dataLen);
			if(!dest->interleavedData.data)
				return false;
		}
	}
	uint32_t tileCount = 0;
	{
		std::unique_lock<std::mutex> lk(interleaveMutex_);
		tileCount = ++strip->tileCounter;
		if(!dest->compositeInterleaved(src))
			return false;
	}

	if(tileCount == numTilesWidth_)
	{
		auto buf = GrkSerializeBuf(dest->interleavedData);
		buf.index = stripId;
		buf.dataLen = dataLen;
		dest->interleavedData.data = nullptr;
		std::queue<GrkSerializeBuf> buffersToSerialize;
		{
			std::unique_lock<std::mutex> lk(heapMutex_);
			// 1. push to heap
			serializeHeap.push(buf);
			// 2. get all sequential buffers in heap
			buf = serializeHeap.pop();
			while(buf.data)
			{
				buffersToSerialize.push(buf);
				buf = serializeHeap.pop();
			}
		}
		// 3. serialize buffers
		if(!buffersToSerialize.empty())
		{
			{
				std::unique_lock<std::mutex> lk(serializeMutex_);
				while(!buffersToSerialize.empty())
				{
					auto b = buffersToSerialize.front();
					if(!serializeBufferCallback_(b, serializeUserData_))
						break;
					buffersToSerialize.pop();
				}
			}
			// if non empty, then there has been a serialize failure
			if(!buffersToSerialize.empty())
			{
				// cleanup
				while(!buffersToSerialize.empty())
				{
					auto b = buffersToSerialize.front();
					b.dealloc();
					buffersToSerialize.pop();
				}
				return false;
			}
		}
	}

	return true;
}
/***
 * Get buffer from pool
 *
 * not thread safe, but it is always called from within a lock
 */
GrkSerializeBuf StripCache::getBufferFromPool(uint64_t len)
{
	for(auto iter = pool.begin(); iter != pool.end(); ++iter)
	{
		if(iter->second.allocLen >= len)
		{
			auto b = iter->second;
			b.dataLen = len;
			pool.erase(iter);
			return b;
		}
	}
	GrkSerializeBuf rc;
	rc.alloc(len);

	return rc;
}
/***
 * return buffer to pool
 *
 * thread-safe
 */
void StripCache::returnBufferToPool(GrkSerializeBuf b)
{
	std::unique_lock<std::mutex> lk(poolMutex_);
	assert(b.data);
	assert(pool.find(b.data) == pool.end());
	pool[b.data] = b;
}

} // namespace grk
