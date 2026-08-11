#include "echion/echion_sampler.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

InterpreterInfo
interpreter(uintptr_t address, uint64_t generation, bool valid = true)
{
    InterpreterInfo info;
    info.address = address;
    info.code_object_generation = generation;
    info.code_object_generation_valid = valid;
    return info;
}

void
store_frame(EchionSampler& echion, const Frame::Key& key, StringTable::Key name)
{
    auto frame = std::make_unique<Frame>(name);
    frame->cache_key = key;
    echion.frame_cache().store(key, std::move(frame));
}

bool
has_frame(EchionSampler& echion, const Frame::Key& key)
{
    return static_cast<bool>(echion.frame_cache().lookup(key));
}

} // namespace

TEST(FrameCache, HashCollisionStillUsesFullKeyEquality)
{
    const Frame::Key first{ 0x1000, 0, 0 };
    const Frame::Key second{ first.code ^ 2654435761ULL, 1, 0 };
    ASSERT_NE(first, second);
    ASSERT_EQ(FrameKeyHash{}(first), FrameKeyHash{}(second));

    LRUCache<Frame::Key, Frame, FrameKeyHash> cache(2);
    cache.store(first, std::make_unique<Frame>(10));
    cache.store(second, std::make_unique<Frame>(20));

    ASSERT_TRUE(cache.lookup(first));
    EXPECT_EQ(cache.lookup(first)->get().name, 10);
    ASSERT_TRUE(cache.lookup(second));
    EXPECT_EQ(cache.lookup(second)->get().name, 20);
}

TEST(FrameCache, UnchangedGenerationPreservesEntries)
{
    EchionSampler echion(2);
    const std::vector<InterpreterInfo> snapshot{ interpreter(0x1000, 7) };
    const Frame::Key key{ 0x2000, 3, 10 };

    echion.update_frame_cache_generations(snapshot, true);
    store_frame(echion, key, 10);
    echion.asyncio_frame_cache_key() = key;

    echion.update_frame_cache_generations(snapshot, true);

    EXPECT_TRUE(echion.persistent_frame_cache_enabled());
    EXPECT_TRUE(has_frame(echion, key));
    EXPECT_EQ(echion.asyncio_frame_cache_key(), key);
}

TEST(FrameCache, GenerationChangeInvalidatesIdentityState)
{
    EchionSampler echion(2);
    const Frame::Key key{ 0x2000, 3, 10 };

    echion.update_frame_cache_generations({ interpreter(0x1000, 7) }, true);
    store_frame(echion, key, 10);
    echion.asyncio_frame_cache_key() = key;
    echion.uvloop_frame_cache_key() = key;

    echion.update_frame_cache_generations({ interpreter(0x1000, 8) }, true);

    EXPECT_TRUE(echion.persistent_frame_cache_enabled());
    EXPECT_FALSE(has_frame(echion, key));
    EXPECT_FALSE(echion.asyncio_frame_cache_key());
    EXPECT_FALSE(echion.uvloop_frame_cache_key());
}

TEST(FrameCache, InterpreterSetChangeInvalidatesEntries)
{
    EchionSampler echion(2);
    const Frame::Key key{ 0x2000, 3, 10 };

    echion.update_frame_cache_generations({ interpreter(0x1000, 7) }, true);
    store_frame(echion, key, 10);
    echion.update_frame_cache_generations({ interpreter(0x1000, 7), interpreter(0x3000, 1) }, true);
    EXPECT_FALSE(has_frame(echion, key));

    store_frame(echion, key, 10);
    echion.update_frame_cache_generations({ interpreter(0x1000, 7) }, true);
    EXPECT_FALSE(has_frame(echion, key));
}

TEST(FrameCache, InvalidSnapshotDisablesPersistentHits)
{
    EchionSampler echion(2);
    const Frame::Key key{ 0x2000, 3, 10 };

    echion.update_frame_cache_generations({ interpreter(0x1000, 7) }, true);
    store_frame(echion, key, 10);
    echion.asyncio_frame_cache_key() = key;
    echion.uvloop_frame_cache_key() = key;

    echion.update_frame_cache_generations({ interpreter(0x1000, 0, false) }, true);

    EXPECT_FALSE(echion.persistent_frame_cache_enabled());
    EXPECT_FALSE(has_frame(echion, key));
    EXPECT_FALSE(echion.asyncio_frame_cache_key());
    EXPECT_FALSE(echion.uvloop_frame_cache_key());
}

TEST(FrameCache, ForkResetAbandonsInheritedIdentityState)
{
    EchionSampler echion(2);
    const Frame::Key key{ 0x2000, 3, 10 };

    echion.update_frame_cache_generations({ interpreter(0x1000, 7) }, true);
    store_frame(echion, key, 10);
    echion.asyncio_frame_cache_key() = key;
    echion.uvloop_frame_cache_key() = key;

    echion.postfork_child();

    EXPECT_FALSE(echion.persistent_frame_cache_enabled());
    EXPECT_FALSE(has_frame(echion, key));
    EXPECT_FALSE(echion.asyncio_frame_cache_key());
    EXPECT_FALSE(echion.uvloop_frame_cache_key());
}
