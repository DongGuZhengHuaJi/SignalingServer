#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct Room {
    std::string room_id;
    std::string meeting_type; // quick | reserved | screen_share
    std::set<std::string> participants;
    std::set<std::string> current_session_ids;
    std::string host_id;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point empty_time;

    Room(std::string room_id_, std::chrono::system_clock::time_point start_time_ = std::chrono::system_clock::now())
        : room_id(std::move(room_id_)), meeting_type("quick"), start_time(start_time_), empty_time() {}
};

enum class JoinRoomResult {
    kOk,
    kRoomNotFound,
    kAlreadyInRoom,
    kBeforeStartTime
};

class RoomManager {
public:
    static RoomManager& getInstance() {
        static RoomManager instance;
        return instance;
    }

    bool createRoom(const std::string& room_id,
                    const std::string& host_id,
                    const std::chrono::system_clock::time_point& start_time,
                    const std::string& meeting_type) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        if (_rooms.find(room_id) != _rooms.end()) {
            return false;
        }
        Room room(room_id, start_time);
        room.host_id = host_id;
        room.meeting_type = meeting_type;
        if (!host_id.empty()) {
            room.participants.insert(host_id);
            room.current_session_ids.insert(host_id);
        }
        _rooms.emplace(room_id, std::move(room));
        return true;
    }

    void createOrUpdateReservedRoom(const std::string& room_id,
                                    const std::string& host_id,
                                    const std::chrono::system_clock::time_point& start_time) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        auto it = _rooms.find(room_id);
        if (it == _rooms.end()) {
            Room room(room_id, start_time);
            room.meeting_type = "reserved";
            room.host_id = host_id;
            if (!host_id.empty()) {
                room.participants.insert(host_id);
            }
            _rooms.emplace(room_id, std::move(room));
            return;
        }

        it->second.start_time = start_time;
        it->second.meeting_type = "reserved";
        if (!host_id.empty()) {
            it->second.host_id = host_id;
            it->second.participants.insert(host_id);
        }
    }

    JoinRoomResult addSession(const std::string& room_id, const std::string& user_id) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        auto it = _rooms.find(room_id);
        if (it == _rooms.end()) {
            return JoinRoomResult::kRoomNotFound;
        }
        if (it->second.start_time > std::chrono::system_clock::now()) {
            return JoinRoomResult::kBeforeStartTime;
        }
        if (it->second.current_session_ids.find(user_id) != it->second.current_session_ids.end()) {
            return JoinRoomResult::kAlreadyInRoom;
        }

        it->second.current_session_ids.insert(user_id);
        if (!user_id.empty()) {
            it->second.participants.insert(user_id);
        }
        it->second.empty_time = std::chrono::system_clock::time_point();
        return JoinRoomResult::kOk;
    }

    bool removeSession(const std::string& room_id, const std::string& user_id, bool* became_empty = nullptr) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        auto it = _rooms.find(room_id);
        if (it == _rooms.end()) {
            return false;
        }

        it->second.current_session_ids.erase(user_id);
        const bool is_empty = it->second.current_session_ids.empty();
        if (is_empty) {
            it->second.empty_time = std::chrono::system_clock::now();
        }
        if (became_empty) {
            *became_empty = is_empty;
        }
        return true;
    }

    std::optional<Room> getRoomCopy(const std::string& room_id) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        auto it = _rooms.find(room_id);
        if (it == _rooms.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<std::string> getSessionIds(const std::string& room_id) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        auto it = _rooms.find(room_id);
        if (it == _rooms.end()) {
            return {};
        }

        return std::vector<std::string>(it->second.current_session_ids.begin(), it->second.current_session_ids.end());
    }

    bool removeRoom(const std::string& room_id, Room* removed_room = nullptr) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        auto it = _rooms.find(room_id);
        if (it == _rooms.end()) {
            return false;
        }

        if (removed_room) {
            *removed_room = it->second;
        }
        _rooms.erase(it);
        return true;
    }

    std::vector<std::string> listExpiredEmptyRooms(std::chrono::minutes max_idle) {
        std::lock_guard<std::recursive_mutex> lock(_map_mutex);
        std::vector<std::string> expired;
        const auto now = std::chrono::system_clock::now();
        for (const auto& [room_id, room] : _rooms) {
            if (room.empty_time != std::chrono::system_clock::time_point() &&
                now - room.empty_time > max_idle) {
                expired.push_back(room_id);
            }
        }
        return expired;
    }

private:
    RoomManager() = default;
    ~RoomManager() = default;
    RoomManager(const RoomManager&) = delete;
    RoomManager& operator=(const RoomManager&) = delete;

    std::unordered_map<std::string, Room> _rooms;
    std::recursive_mutex _map_mutex;
};