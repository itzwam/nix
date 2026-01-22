#include "nix/util/nar-cache.hh"
#include "nix/util/file-system.hh"

#include <nlohmann/json.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace nix {

ref<NarAccessor> MemoryNarCache::getOrInsert(const Hash & narHash, std::function<void(Sink &)> populate)
{
    // Check in-memory cache first
    if (auto * accessor = get(nars, narHash))
        return *accessor;

    StringSink sink;
    populate(sink);
    auto accessor = makeNarAccessor(std::move(sink.s));
    nars.emplace(narHash, accessor);
    return accessor;
}

LocalNarCache::LocalNarCache(std::filesystem::path cacheDir)
    : cacheSink(false)
{
    createDirs(cacheDir);
    cacheSink.dstPath = std::move(cacheDir);
}

ref<NarAccessor> LocalNarCache::getOrInsert(const Hash & narHash, std::function<void(Sink &)> populate)
{
    // Check in-memory cache first
    if (auto * accessor = get(nars, narHash))
        return *accessor;

    auto cacheAccessor = [&](ref<NarAccessor> accessor) {
        nars.emplace(narHash, accessor);
        return accessor;
    };

    auto makeCacheFile = [&](const std::string & ext) -> CanonPath {
        return {narHash.to_string(HashFormat::Nix32, false) + "." + ext};
    };

    auto cacheFile = makeCacheFile("nar");
    auto listingFile = makeCacheFile("ls");

    auto cacheFilePath = cacheSink.dstPath / cacheFile.rel();

    if (nix::pathExists(cacheFilePath)) {
        auto listingFilePath = cacheSink.dstPath / listingFile.rel();

        try {
            return cacheAccessor(makeLazyNarAccessor(
                nlohmann::json::parse(nix::readFile(listingFilePath)).template get<NarListing>(),
                seekableGetNarBytes(cacheFilePath)));
        } catch (SystemError &) {
        }

        try {
            return cacheAccessor(makeNarAccessor(nix::readFile(cacheFilePath)));
        } catch (SystemError &) {
        }
    }

    NarListing listing;
    try {
        /* FIXME: do this asynchronously. */
        cacheSink.createRegularFile(cacheFile, [&](CreateRegularFileSink & fileSink) {
            auto source = sinkToSource([&](Sink & parseSink) {
                TeeSink teeSink{fileSink, parseSink};
                populate(teeSink);
            });
            listing = parseNarListing(*source);
        });
    } catch (...) {
        ignoreExceptionExceptInterrupt();
        StringSink narSink;
        populate(narSink);
        return cacheAccessor(makeNarAccessor(std::move(narSink.s)));
    }

    try {
        cacheSink.createRegularFile(listingFile, [&](CreateRegularFileSink & sink) {
            auto s = nlohmann::json(listing).dump();
            StringSource source{s};
            source.drainInto(sink);
        });
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }

    return cacheAccessor(makeLazyNarAccessor(std::move(listing), seekableGetNarBytes(cacheFilePath)));
}

} // namespace nix
