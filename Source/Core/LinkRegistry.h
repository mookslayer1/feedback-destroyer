#pragma once
#include <array>
#include <atomic>

namespace fd
{
enum class LinkRole { Channel = 0, Master = 1 };

// STAGE 2 STUB. Process-wide registry that lets plugin instances in the same host process
// find each other (like the Waves NLS linking, but done by hand since VST3 has no
// inter-instance API). Right now it only counts members per group so you can verify,
// inside SuperRack Performer, that all your instances really share one process.
// Next step: add per-instance published peak lists (seqlock / SPSC, no locks on the audio
// thread) and the master-vs-channel attribution logic.
class LinkRegistry
{
public:
    static constexpr int kGroups = 8; // groups are 1..8; 0 means "Off"

    static LinkRegistry& get()
    {
        static LinkRegistry instance;
        return instance;
    }

    // Atomics only: safe from any thread.
    void join (int group, LinkRole role) noexcept  { counter (group, role).fetch_add (1); }
    void leave (int group, LinkRole role) noexcept { counter (group, role).fetch_sub (1); }
    int numChannels (int group) const noexcept { return counts_[idx (group, LinkRole::Channel)].load(); }
    int numMasters (int group) const noexcept  { return counts_[idx (group, LinkRole::Master)].load(); }

private:
    static int idx (int group, LinkRole r) noexcept { return ((group - 1) & (kGroups - 1)) * 2 + (int) r; }
    std::atomic<int>& counter (int g, LinkRole r) noexcept { return counts_[(size_t) idx (g, r)]; }

    std::array<std::atomic<int>, kGroups * 2> counts_ {};
};
} // namespace fd
