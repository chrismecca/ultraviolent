#include <ultraviolent/backends/file_block_store.hpp>

namespace ultraviolent::backends {

std::unique_ptr<FileBlockStore> FileBlockStore::open(const std::string& path, bool writable) {
    const auto mode = std::ios::binary | std::ios::in | (writable ? std::ios::out : std::ios::in);
    std::fstream stream{path, mode};
    if (!stream) {
        return nullptr;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return nullptr;
    }
    return std::unique_ptr<FileBlockStore>{
        new FileBlockStore{std::move(stream), static_cast<std::uint64_t>(size), writable}};
}

bool FileBlockStore::read(std::uint64_t offset, std::span<std::byte> bytes) {
    if (offset > size_ || bytes.size() > size_ - offset) {
        return false;
    }
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream_);
}

bool FileBlockStore::write(std::uint64_t offset, std::span<const std::byte> bytes) {
    if (!writable_ || offset > size_ || bytes.size() > size_ - offset) {
        return false;
    }
    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset));
    stream_.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream_);
}

} // namespace ultraviolent::backends
