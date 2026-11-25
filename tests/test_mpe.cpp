/*
 * MPE channel allocation: every sounding note gets a member channel, freed on
 * release, and the oldest is stolen when all fifteen are busy.
 */
#include <guitarmidi/notepostprocess.hpp>

#include <cstdio>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expression);
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

} // namespace

int main()
{
    using GuitarMidi::MpeChannelPool;

    MpeChannelPool pool;
    std::vector<int> channels;
    constexpr int kMembers = MpeChannelPool::kLastMember - MpeChannelPool::kFirstMember + 1;
    for (int i = 0; i < kMembers; ++i)
    {
        const int channel = pool.allocate();
        CHECK(channel >= MpeChannelPool::kFirstMember);
        CHECK(channel <= MpeChannelPool::kLastMember);
        for (int other : channels)
            CHECK(other != channel);
        channels.push_back(channel);
    }
    CHECK(static_cast<int>(channels.size()) == kMembers);

    // All busy: the next allocation steals the oldest, which was channels[0].
    const int stolen = pool.allocate();
    CHECK(stolen == channels[0]);

    // A released channel is reused before anything is stolen.
    const int freed = channels[1];
    pool.release(freed);
    CHECK(pool.allocate() == freed);

    // Out-of-range releases are ignored.
    pool.release(0);
    pool.release(17);

    pool.reset();
    const int afterReset = pool.allocate();
    CHECK(afterReset == MpeChannelPool::kFirstMember);

    if (failures == 0)
        std::printf("test_mpe: OK\n");
    else
        std::printf("test_mpe: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
