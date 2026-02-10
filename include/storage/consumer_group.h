#pragma once
#include "core/types.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace blaze {
namespace storage {

enum class GroupState { Empty, PreparingRebalance, CompletingRebalance, Stable, Dead };

static const char* state_name(GroupState s) {
    switch (s) {
        case GroupState::Empty: return "Empty";
        case GroupState::PreparingRebalance: return "PreparingRebalance";
        case GroupState::CompletingRebalance: return "CompletingRebalance";
        case GroupState::Stable: return "Stable";
        case GroupState::Dead: return "Dead";
    }
    return "?";
}

struct MemberMetadata {
    std::string member_id;
    std::string client_id;
    std::string protocol_type;
    std::vector<JoinGroupProtocol> protocols;
    std::vector<uint8_t> assignment;
    int32_t session_timeout_ms = 10000;
    int32_t rebalance_timeout_ms = 300000;
    int64_t last_heartbeat_ms = 0;
};

struct ConsumerGroup {
    std::string group_id;
    GroupState state = GroupState::Empty;
    int32_t generation_id = 0;
    std::string protocol_type;
    std::string protocol_name;
    std::string leader_id;
    std::unordered_map<std::string, MemberMetadata, StringHash> members;
    std::unordered_map<TopicPartition, int64_t, TopicPartitionHash> committed_offsets;
    std::unordered_map<TopicPartition, std::string, TopicPartitionHash> offset_metadata;
};

class ConsumerGroupManager {
public:
    JoinGroupResponse join_group(const JoinGroupRequest& req) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& group = groups_[req.group_id];
        group.group_id = req.group_id;

        JoinGroupResponse resp;
        std::string member_id = req.member_id;

        std::cout << "  [GRP] JoinGroup group=" << req.group_id
                  << " member_id=\"" << req.member_id << "\""
                  << " state=" << state_name(group.state)
                  << " gen=" << group.generation_id << "\n" << std::flush;

        // Phase 1: empty member_id → generate one and return it
        // rdkafka expects error_code=0 with the generated member_id on first join,
        // then re-sends JoinGroup with that member_id. However, some clients
        // (including kcat/rdkafka) treat a successful first-join response as the
        // actual join. So we handle both patterns: if member_id is empty, we
        // generate one and proceed with the join (not requiring a re-join).
        if (member_id.empty()) {
            member_id = generate_member_id(req.client_id);
            std::cout << "  [GRP]   Generated member_id=" << member_id << "\n" << std::flush;
        } else {
            // Known member re-joining while group is Stable with same generation:
            // don't trigger a new rebalance, just return current state
            if (group.state == GroupState::Stable &&
                group.members.find(member_id) != group.members.end()) {
                auto& member = group.members[member_id];
                member.last_heartbeat_ms = now_ms();
                member.protocols = req.protocols;

                resp.error_code = 0;
                resp.generation_id = group.generation_id;
                resp.protocol_name = group.protocol_name;
                resp.leader_id = group.leader_id;
                resp.member_id = member_id;

                if (member_id == group.leader_id) {
                    for (const auto& [mid, meta] : group.members) {
                        JoinGroupMember m;
                        m.member_id = mid;
                        for (const auto& p : meta.protocols) {
                            if (p.name == group.protocol_name) {
                                m.metadata = p.metadata;
                                break;
                            }
                        }
                        resp.members.push_back(std::move(m));
                    }
                }

                std::cout << "  [GRP]   Re-join stable group, no rebalance\n" << std::flush;
                return resp;
            }

            // Validate existing member_id if group has members
            if (!group.members.empty() &&
                group.state != GroupState::Empty &&
                group.members.find(member_id) == group.members.end()) {
                resp.error_code = 25; // UNKNOWN_MEMBER_ID
                resp.member_id = member_id;
                resp.generation_id = -1;
                std::cout << "  [GRP]   UNKNOWN_MEMBER_ID\n" << std::flush;
                return resp;
            }
        }

        // Add/update member
        auto& member = group.members[member_id];
        member.member_id = member_id;
        member.client_id = req.client_id;
        member.protocol_type = req.protocol_type;
        member.protocols = req.protocols;
        member.session_timeout_ms = req.session_timeout_ms;
        member.rebalance_timeout_ms = req.rebalance_timeout_ms;
        member.last_heartbeat_ms = now_ms();

        if (group.protocol_type.empty()) {
            group.protocol_type = req.protocol_type;
        }

        // Trigger rebalance
        group.generation_id++;
        group.protocol_name = choose_protocol(group);

        if (group.leader_id.empty() || group.members.find(group.leader_id) == group.members.end()) {
            group.leader_id = member_id;
        }

        group.state = GroupState::CompletingRebalance;

        resp.error_code = 0;
        resp.generation_id = group.generation_id;
        resp.protocol_name = group.protocol_name;
        resp.leader_id = group.leader_id;
        resp.member_id = member_id;

        // Leader gets the full member list
        if (member_id == group.leader_id) {
            for (const auto& [mid, meta] : group.members) {
                JoinGroupMember m;
                m.member_id = mid;
                for (const auto& p : meta.protocols) {
                    if (p.name == group.protocol_name) {
                        m.metadata = p.metadata;
                        break;
                    }
                }
                resp.members.push_back(std::move(m));
            }
        }

        std::cout << "  [GRP]   Rebalance triggered, gen=" << group.generation_id
                  << " leader=" << group.leader_id
                  << " members=" << group.members.size() << "\n" << std::flush;

        return resp;
    }

    SyncGroupResponse sync_group(const SyncGroupRequest& req) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = groups_.find(req.group_id);
        SyncGroupResponse resp;

        std::cout << "  [GRP] SyncGroup group=" << req.group_id
                  << " member=" << req.member_id
                  << " gen=" << req.generation_id
                  << " assignments=" << req.assignments.size() << "\n" << std::flush;

        if (it == groups_.end()) {
            resp.error_code = 25;
            return resp;
        }

        auto& group = it->second;

        if (group.members.find(req.member_id) == group.members.end()) {
            resp.error_code = 25;
            return resp;
        }

        if (req.generation_id != group.generation_id) {
            resp.error_code = 22;
            return resp;
        }

        // Leader provides assignments
        if (req.member_id == group.leader_id && !req.assignments.empty()) {
            for (const auto& a : req.assignments) {
                auto mit = group.members.find(a.member_id);
                if (mit != group.members.end()) {
                    mit->second.assignment = a.assignment;
                }
            }
            group.state = GroupState::Stable;
            std::cout << "  [GRP]   Leader synced, state -> Stable\n" << std::flush;
        }

        auto mit = group.members.find(req.member_id);
        if (mit != group.members.end()) {
            resp.member_assignment = mit->second.assignment;
        }
        resp.error_code = 0;

        std::cout << "  [GRP]   Returning assignment size=" << resp.member_assignment.size() << "\n" << std::flush;
        return resp;
    }

    int16_t heartbeat(const HeartbeatRequest& req) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = groups_.find(req.group_id);
        if (it == groups_.end()) return 25;

        auto& group = it->second;
        auto mit = group.members.find(req.member_id);
        if (mit == group.members.end()) return 25;
        if (req.generation_id != group.generation_id) return 22;

        mit->second.last_heartbeat_ms = now_ms();

        check_session_timeouts(group);

        if (group.state == GroupState::PreparingRebalance ||
            group.state == GroupState::CompletingRebalance) {
            return 27; // REBALANCE_IN_PROGRESS
        }

        return 0;
    }

    int16_t leave_group(const LeaveGroupRequest& req) {
        std::lock_guard<std::mutex> lock(mu_);
        std::cout << "  [GRP] LeaveGroup group=" << req.group_id
                  << " member=" << req.member_id << "\n" << std::flush;

        auto it = groups_.find(req.group_id);
        if (it == groups_.end()) return 0;

        auto& group = it->second;
        auto mit = group.members.find(req.member_id);
        if (mit == group.members.end()) return 25;

        group.members.erase(mit);

        if (group.members.empty()) {
            group.state = GroupState::Empty;
            group.generation_id++;
            group.leader_id.clear();
        } else {
            if (req.member_id == group.leader_id) {
                group.leader_id = group.members.begin()->first;
            }
            group.generation_id++;
            group.state = GroupState::PreparingRebalance;
        }

        return 0;
    }

    std::vector<OffsetCommitPartitionResponse> offset_commit(const OffsetCommitRequest& req) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& group = groups_[req.group_id];
        std::vector<OffsetCommitPartitionResponse> responses;

        for (const auto& p : req.partitions) {
            group.committed_offsets[p.tp] = p.offset;
            group.offset_metadata[p.tp] = p.metadata;
            OffsetCommitPartitionResponse r;
            r.tp = p.tp;
            r.error_code = 0;
            responses.push_back(std::move(r));
        }
        std::cout << "  [GRP] OffsetCommit group=" << req.group_id
                  << " partitions=" << req.partitions.size() << "\n" << std::flush;
        return responses;
    }

    std::vector<OffsetFetchPartitionResponse> offset_fetch(const OffsetFetchRequest& req) {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<OffsetFetchPartitionResponse> responses;
        auto it = groups_.find(req.group_id);

        for (const auto& p : req.partitions) {
            OffsetFetchPartitionResponse r;
            r.tp = p.tp;
            if (it != groups_.end()) {
                auto oit = it->second.committed_offsets.find(p.tp);
                if (oit != it->second.committed_offsets.end()) {
                    r.offset = oit->second;
                    auto mit = it->second.offset_metadata.find(p.tp);
                    if (mit != it->second.offset_metadata.end()) {
                        r.metadata = mit->second;
                    }
                    r.error_code = 0;
                } else {
                    r.offset = -1;
                    r.error_code = 0;
                }
            } else {
                r.offset = -1;
                r.error_code = 0;
            }
            responses.push_back(std::move(r));
        }
        std::cout << "  [GRP] OffsetFetch group=" << req.group_id
                  << " partitions=" << req.partitions.size() << "\n" << std::flush;
        return responses;
    }

private:
    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static std::string generate_member_id(const std::string& client_id) {
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<uint32_t> dist;
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08x", dist(rng));
        return client_id + "-" + hex;
    }

    static std::string choose_protocol(const ConsumerGroup& group) {
        std::unordered_map<std::string, int, StringHash> votes;
        for (const auto& [_, member] : group.members) {
            for (const auto& p : member.protocols) {
                votes[p.name]++;
            }
        }
        int num_members = static_cast<int>(group.members.size());
        for (const auto& [_, member] : group.members) {
            for (const auto& p : member.protocols) {
                if (votes[p.name] == num_members) return p.name;
            }
        }
        if (!group.members.empty() && !group.members.begin()->second.protocols.empty()) {
            return group.members.begin()->second.protocols[0].name;
        }
        return "";
    }

    void check_session_timeouts(ConsumerGroup& group) {
        int64_t now = now_ms();
        std::vector<std::string> expired;
        for (const auto& [mid, member] : group.members) {
            if (member.last_heartbeat_ms > 0 &&
                (now - member.last_heartbeat_ms) > member.session_timeout_ms) {
                expired.push_back(mid);
            }
        }
        for (const auto& mid : expired) {
            group.members.erase(mid);
        }
        if (!expired.empty()) {
            if (group.members.empty()) {
                group.state = GroupState::Empty;
                group.leader_id.clear();
            } else {
                if (group.members.find(group.leader_id) == group.members.end()) {
                    group.leader_id = group.members.begin()->first;
                }
                group.generation_id++;
                group.state = GroupState::PreparingRebalance;
            }
        }
    }

    std::mutex mu_;
    std::unordered_map<std::string, ConsumerGroup, StringHash> groups_;
};

} // namespace storage
} // namespace blaze
