#include <runtime/assets/asset_store.h>

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <runtime/anim/skeletal_clips.h>

#include <unordered_map>

namespace opennova::assets {
namespace {

std::string file_name(const std::string &name, const char *extension, bool replace = false) {
    std::string key = name.substr(name.find_last_of("/\\") + 1);
    if (key.empty()) return {};
    if (replace) {
        const auto dot = key.find_last_of('.');
        if (dot != std::string::npos) key.resize(dot);
    }
    if (key.empty()) return {};
    if (!strutil::ends_with_icase(key, extension)) key += extension;
    return strutil::to_lower(key);
}

template<class T, void (*Free)(T *)>
std::shared_ptr<T> parsed_owner() {
    return std::shared_ptr<T>(new T{}, [](T *value) { Free(value); delete value; });
}

void key_part(std::string &key, const std::string &part) {
    key += std::to_string(part.size()) + ':';
    key += part;
}

void rig_bones_key(std::string &key, const std::vector<anim::Vec3> &origins,
        const std::vector<int> &parents) {
    key_part(key, std::to_string(origins.size()));
    for (const auto &v : origins) {
        key.append(reinterpret_cast<const char *>(&v.x), sizeof(v.x));
        key.append(reinterpret_cast<const char *>(&v.y), sizeof(v.y));
        key.append(reinterpret_cast<const char *>(&v.z), sizeof(v.z));
    }
    key_part(key, std::to_string(parents.size()));
    for (int parent : parents)
        key.append(reinterpret_cast<const char *>(&parent), sizeof(parent));
}

} // namespace

struct AssetStore::Impl {
    const ResourceIndex *index = nullptr;
    uint64_t source_revision = 0;
    uint64_t epoch = 0;
    std::unordered_map<std::string, Model> models;
    std::unordered_map<std::string, AnimationMap> maps;
    std::unordered_map<std::string, BoneAnimation> animations;
    std::unordered_map<std::string, SkeletalRig> rigs;
};

AssetStore::AssetStore(const ResourceIndex *index) : impl_(std::make_unique<Impl>()) {
    impl_->index = index;
    invalidate();
}
AssetStore::~AssetStore() = default;
const ResourceIndex *AssetStore::index() const { return impl_->index; }
bool AssetStore::has_source() const { return impl_->index && !impl_->index->root_dir().empty(); }

void AssetStore::invalidate() const {
    impl_->rigs.clear();
    impl_->animations.clear();
    impl_->maps.clear();
    impl_->models.clear();
    impl_->source_revision = impl_->index ? impl_->index->revision() : 0;
    impl_->epoch = cache_epoch();
}

void AssetStore::sync_source() const {
    if (impl_->epoch != cache_epoch() ||
            (impl_->index && impl_->source_revision != impl_->index->revision())) invalidate();
}

Model AssetStore::model(const std::string &graphic) const {
    sync_source();
    const std::string key = file_name(graphic, ".3di", true);
    if (key.empty()) return {};
    const auto found = impl_->models.find(key);
    if (found != impl_->models.end()) return found->second;
    Model result;
    std::vector<uint8_t> bytes;
    if (impl_->index && impl_->index->read_file(key, bytes) && !bytes.empty()) {
        auto parsed = parsed_owner<threedi::Threedi3di3, threedi::threedi_3di3_free>();
        if (threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), parsed.get()) == 0)
            result = std::move(parsed);
    }
    impl_->models.emplace(key, result);
    return result;
}

AnimationMap AssetStore::animation_map(const std::string &name) const {
    sync_source();
    const std::string key = file_name(name, ".adm");
    if (key.empty()) return {};
    const auto found = impl_->maps.find(key);
    if (found != impl_->maps.end()) return found->second;
    AnimationMap result;
    std::vector<uint8_t> bytes;
    if (impl_->index && impl_->index->read_file(key, bytes) && !bytes.empty()) {
        auto parsed = parsed_owner<adm::AdmFile, adm::adm_free>();
        if (adm::adm_parse_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(), parsed.get()) == 0)
            result = std::move(parsed);
    }
    impl_->maps.emplace(key, result);
    return result;
}

BoneAnimation AssetStore::bone_animation(const std::string &name) const {
    sync_source();
    const std::string key = file_name(name, ".bad");
    if (key.empty()) return {};
    const auto found = impl_->animations.find(key);
    if (found != impl_->animations.end()) return found->second;
    BoneAnimation result;
    std::vector<uint8_t> bytes;
    if (impl_->index && impl_->index->read_file(key, bytes) && !bytes.empty()) {
        auto parsed = parsed_owner<bad::BadFile, bad::bad_free>();
        if (bad::bad_parse_buffer(bytes.data(), bytes.size(), parsed.get()) == 0)
            result = std::move(parsed);
    }
    impl_->animations.emplace(key, result);
    return result;
}

SkeletalRig AssetStore::skeletal_rig(const std::string &adm_name,
        const std::vector<anim::Vec3> &origins, const std::vector<int> &parents) const {
    sync_source();
    const auto name = file_name(adm_name, ".adm");
    if (name.empty()) return {};
    std::string key = "adm:";
    key_part(key, name);
    rig_bones_key(key, origins, parents);
    const auto found = impl_->rigs.find(key);
    if (found != impl_->rigs.end()) return found->second;
    auto rig = std::make_shared<anim::SkeletalClips>();
    SkeletalRig result;
    if (rig->load_from_adm(this, name, origins, parents)) result = std::move(rig);
    impl_->rigs.emplace(std::move(key), result);
    return result;
}

SkeletalRig AssetStore::skeletal_rig_from_files(const std::string &skeleton_bad,
        const std::vector<std::pair<std::string, std::string>> &clips,
        const std::vector<anim::Vec3> &origins, const std::vector<int> &parents) const {
    sync_source();
    const auto name = file_name(skeleton_bad, ".bad");
    if (name.empty()) return {};
    std::string key = "files:";
    key_part(key, name);
    key_part(key, std::to_string(clips.size()));
    for (const auto &clip : clips) {
        key_part(key, clip.first); // diagnostic keys preserve authored spelling
        key_part(key, file_name(clip.second, ".bad"));
    }
    rig_bones_key(key, origins, parents);
    const auto found = impl_->rigs.find(key);
    if (found != impl_->rigs.end()) return found->second;
    auto rig = std::make_shared<anim::SkeletalClips>();
    SkeletalRig result;
    if (rig->load_from_files(this, name, clips, origins, parents)) result = std::move(rig);
    impl_->rigs.emplace(std::move(key), result);
    return result;
}

Model read_model_file(const std::string &path) {
    auto parsed = parsed_owner<threedi::Threedi3di3, threedi::threedi_3di3_free>();
    if (threedi::threedi_3di3_read(path.c_str(), parsed.get()) != 0) return {};
    return parsed;
}

} // namespace opennova::assets
