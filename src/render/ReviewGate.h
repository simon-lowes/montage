// Montage — who may have a passphrase tried by a browser review now (render/WebRtcReview.cpp asks before it compares
// one). Kept apart from the web server so that it can be run against a clock of the caller's.
//
// An address (an IPv6 /64) that has guessed wrong too often lately is refused untried, and each address may ask only
// so often a second, except addresses that gave the right passphrase lately: guessing elsewhere never shuts reviewers
// already in out. A network (an IPv4 /24, an IPv6 /48) is rationed from its first wrong guess: it has a few tries at
// once (fewer when very many networks are guessing) and then its share of the few a second that the networks listed
// have between them. A network's guessing therefore uses up nobody else's tries: a newcomer from elsewhere is tried at
// once. The list holds so many networks; past that, the networks not in it have the same few tries a second between
// them, so that guessing from more networks than the list holds gains nothing, and a newcomer is still tried within a
// second or so of the guessers there letting up. A try is used up only by a wrong passphrase: the right one leaves
// its network's ration as it was, for the next reviewer behind the same network. The last try of a ration is kept for
// addresses with no wrong passphrase on record: guessers who keep asking faster than the ration fills have theirs on
// record, so they keep it from reaching more than one try, and that one goes to the next newcomer instead.
#pragma once

#include <QString>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <map>

namespace montage::output {

class PassphraseGate {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr double kGuessRate = 5;                  // passphrases tried a second by the networks listed, and by the rest
    static constexpr double kNetworkBurst = 3;               // and at once from one network that has guessed wrong
    static constexpr double kKeptForNewcomers = 1;           // of which addresses with a wrong one on record leave this many
    static constexpr double kListedBurst = 256;              // (fewer once the networks listed are more than a third of this)
    static constexpr double kPeerRate = 1, kPeerBurst = 8;   // and from one address not yet let in
    static constexpr std::chrono::hours kTrustedMemory{1};   // how long a right passphrase lets its address past the rest
    static constexpr std::chrono::minutes kFailureMemory{15};  // how long a wrong one is held against its address and network
    static constexpr size_t kListed = 4096;                  // addresses and networks remembered

    enum class Answer { Try, Blocked, AddressBusy, NetworkBusy };
    struct Verdict {
        Answer answer = Answer::Try;
        int retryAfter = 0;    // seconds, for the two Busy answers
        bool charged = false;  // a try of its network's ration was taken (given back by right())
    };

    // What has run out is forgotten (ask() does it too).
    void tidy(Clock::time_point now) {
        forget(failures_, [&](const Failures& f) { return now - f.last > kFailureMemory; });
        forget(trusted_, [&](Clock::time_point t) { return now - t > kTrustedMemory; });
        forget(peers_, [&](const Bucket& b) { return now - b.at > std::chrono::minutes(1); });
        forget(listed_, [&](const Ration& n) { return now - n.last > kFailureMemory; });
        while (failures_.size() > kListed)
            failures_.erase(std::min_element(failures_.begin(), failures_.end(), [](const auto& a, const auto& b) { return a.second.last < b.second.last; }));
    }
    // Whether the address gave the right passphrase lately (as of the last tidy()).
    bool trusted(const QString& address) const { return trusted_.count(address) > 0; }

    // Before a passphrase from `address` (in `network`) is compared: Try, or why it is refused untried.
    Verdict ask(const QString& address, const QString& network, Clock::time_point now) {
        tidy(now);
        const bool known = trusted(address);
        // (an address a reviewer is in lately, as a shared network's, is blocked only after many more)
        const auto onRecord = failures_.find(address);
        if (onRecord != failures_.end() && onRecord->second.count >= (known ? 100 : 10)) return {Answer::Blocked, 0, false};
        if (known) return {};
        if (peers_.size() > kListed) peers_.clear();  // (a flood of addresses: they start full again)
        Bucket& mine = peers_.try_emplace(address, Bucket{kPeerBurst, now}).first->second;
        mine.tokens = std::min(kPeerBurst, mine.tokens + seconds(now - mine.at) * kPeerRate);
        mine.at = now;
        if (mine.tokens < 1) return {Answer::AddressBusy, 2, false};
        mine.tokens -= 1;
        // (the network around it, if one there has guessed wrong lately; with the list full, every network not in it)
        double rate = 0;
        Ration* ration = rationOf(network, now, rate);
        if (!ration) return {};
        ration->tokens = std::min(kNetworkBurst, ration->tokens + seconds(now - ration->at) * rate);
        ration->at = now;
        // (an address that has guessed wrong lately leaves the last try for one that has not)
        const double least = onRecord == failures_.end() ? 1 : 1 + kKeptForNewcomers;
        if (ration->tokens < least) return {Answer::NetworkBusy, int(std::clamp(std::ceil((least - ration->tokens) / rate), 1.0, 600.0)), false};
        ration->tokens -= 1;
        return {Answer::Try, 0, true};
    }

    // The passphrase asked about was right: the address is let past the rest for a while, and the try is not used up.
    void right(const QString& address, const QString& network, Clock::time_point now, const Verdict& asked) {
        if (trusted_.size() < kListed || trusted(address)) trusted_[address] = now;
        if (!asked.charged) return;
        double rate = 0;
        if (Ration* ration = rationOf(network, now, rate)) ration->tokens = std::min(kNetworkBurst, ration->tokens + 1);
    }

    // It was wrong: counted against the address, and its network listed. The milliseconds after which the address is
    // to be told so (a second after the one before from the same address).
    int wrong(const QString& address, const QString& network, Clock::time_point now) {
        Failures& f = failures_[address];
        ++f.count;
        f.last = now;
        // (the network's first wrong guess lists it, with the tries it has at once still to use: three while few
        // networks are guessing, fewer and then none as they become many, so that guessing from very many networks
        // gains little more than each one's first try)
        if (const auto listed = listed_.find(network); listed != listed_.end()) {
            listed->second.last = now;
        } else if (listed_.size() < kListed) {
            Ration& n = listed_[network];
            n.last = now;
            n.at = now;
            n.tokens = std::clamp(std::floor(kListedBurst / double(listed_.size())), 0.0, kNetworkBurst);
        }
        // (per address: wrong guesses from elsewhere never hold back anyone's answer, the rations bound how fast all
        // of them together may guess)
        f.nextRefusal = std::max(now, f.nextRefusal) + std::chrono::seconds(1);
        return int(std::chrono::duration_cast<std::chrono::milliseconds>(f.nextRefusal - now).count());
    }

    size_t networksListed() const { return listed_.size(); }

private:
    struct Failures {
        int count = 0;
        Clock::time_point last;
        Clock::time_point nextRefusal;  // (each wrong one from here told a second after the one before)
    };
    struct Bucket {
        double tokens = 0;
        Clock::time_point at;
    };
    // A network that has guessed wrong lately, and the passphrases it may still try.
    struct Ration {
        Clock::time_point last;  // when one there last guessed wrong
        double tokens = kNetworkBurst;
        Clock::time_point at;
    };
    static double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
    template <typename Map, typename Old>
    static void forget(Map& map, Old old) {
        for (auto it = map.begin(); it != map.end();) it = old(it->second) ? map.erase(it) : std::next(it);
    }
    // The ration a network's tries come from, and what it gains a second: its own when it is listed (the listed
    // share the rate evenly); with the list full, the one every network not in it shares, which gains the whole rate
    // by itself (shared with the listed it gained a try in a quarter of an hour, and a newcomer with the right
    // passphrase was refused untried for as long); none for a network not listed while there is room.
    Ration* rationOf(const QString& network, Clock::time_point now, double& rate) {
        if (const auto listed = listed_.find(network); listed != listed_.end()) {
            rate = kGuessRate / double(listed_.size());
            return &listed->second;
        }
        if (listed_.size() < kListed) return nullptr;
        if (!elsewhereBegun_) elsewhere_.at = now, elsewhereBegun_ = true;
        rate = kGuessRate;
        return &elsewhere_;
    }

    std::map<QString, Failures> failures_;         // by address
    std::map<QString, Bucket> peers_;              // by address: its requests
    std::map<QString, Clock::time_point> trusted_;  // addresses that gave the right passphrase lately
    std::map<QString, Ration> listed_;             // by network: those that guessed wrong lately
    Ration elsewhere_;                             // every network not listed, once the list is full
    bool elsewhereBegun_ = false;
};

}  // namespace montage::output
