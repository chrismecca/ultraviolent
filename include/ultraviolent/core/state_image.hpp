#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
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

    // Every field of `other`, with `prefix` before its key: a second instance of a component
    // whose fields have fixed names saves into its own image, merged here.
    void put_image(std::string_view prefix, const StateImage& other);
    // The fields whose keys start with `prefix`, with it removed.
    [[nodiscard]] StateImage sub_image(std::string_view prefix) const;

    [[nodiscard]] std::vector<std::byte> serialize() const;
    [[nodiscard]] static std::optional<StateImage> deserialize(std::span<const std::byte> data);
    // The same from a snapshot of `size` bytes read in order: `read` fills each span it is
    // given, or returns false. A snapshot file then goes straight into its fields, without a
    // copy of the whole file first (guest memory is most of it).
    [[nodiscard]] static std::optional<StateImage>
    deserialize(std::uint64_t size, const std::function<bool(std::span<std::byte>)>& read);

  private:
    // Field storage whose new bytes are left unset rather than zeroed: they are filled at once.
    template <class T> struct FillLater : std::allocator<T> {
        template <class U> struct rebind {
            using other = FillLater<U>;
        };
        FillLater() = default;
        template <class U> FillLater(const FillLater<U>&) noexcept {}
        template <class U> void construct(U* p) {
            ::new (static_cast<void*>(p)) U;
        }
        template <class U, class... Args> void construct(U* p, Args&&... args) {
            ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
        }
    };
    using Bytes = std::vector<std::byte, FillLater<std::byte>>;

    std::map<std::string, Bytes, std::less<>> fields_;
};

} // namespace ultraviolent
