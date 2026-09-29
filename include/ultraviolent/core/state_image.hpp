#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ultraviolent {

// A machine snapshot as named byte fields (ARCHITECTURE "Determinism"; IP27.adoc
// "Snapshots"). Components save and restore their own fields by name, so a snapshot survives
// a model that gained fields since it was taken: missing fields keep their reset values and
// unknown fields are ignored. Values are stored in host representation; a snapshot belongs to
// the host that made it.
//
// Snapshots speed up exploration only. A checkpoint is established by a run from power-on.
class StateImage {
  public:
    void put_bytes(std::string key, std::span<const std::byte> bytes);

    template <class T> void put(std::string key, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        put_bytes(std::move(key), std::as_bytes(std::span{&value, 1}));
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> get_bytes(std::string_view key) const;

    // Copies the field into `value` when it exists with the right size. Returns whether it did.
    template <class T> bool get(std::string_view key, T& value) const {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto bytes = get_bytes(key);
        if (!bytes || bytes->size() != sizeof(T)) {
            return false;
        }
        std::memcpy(&value, bytes->data(), sizeof(T));
        return true;
    }

    // Large, mostly zero storage such as guest memory: stores the 64 KiB pages of `bytes`
    // that are not all zero, as fields `<key>.pages` and `<key>.contents`.
    void put_sparse(const std::string& key, std::span<const std::byte> bytes);
    // Restores storage saved by put_sparse into `bytes`, which must have the saved size
    // (`<key>.size`); pages not saved become zero. Returns whether it did;
    // `bytes` is unchanged when it did not.
    bool get_sparse(const std::string& key, std::span<std::byte> bytes) const;

    [[nodiscard]] std::vector<std::byte> serialize() const;
    [[nodiscard]] static std::optional<StateImage> deserialize(std::span<const std::byte> data);

  private:
    std::map<std::string, std::vector<std::byte>, std::less<>> fields_;
};

} // namespace ultraviolent
