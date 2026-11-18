// Clean-room v1.1.5.5 module mesh, action, skin, sound, music, particle,
// postfx, and Data/{flora_kinds,ground_specs,skyboxes} loaders via Vfs/TokenReader.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/game/module_loader_common.h"
#include "wse/game/module_world_loaders.h"
#include "wse/game/world_state.h"

namespace wse::game {

struct LoadedMesh {
    std::string id{};
    std::uint32_t flags = 0;
    std::string resource_name{};
    std::array<float, 9> transform{};
};

struct LoadedActionAnim {
    float duration = 0.0f;
    std::string anim_name{};
    std::int32_t start_time = 0;
    std::int32_t end_time = 0;
    std::uint32_t flags = 0;
    std::uint32_t sound_times = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float ragdoll_delay = 0.0f;
};

struct LoadedAction {
    std::string id{};
    std::uint32_t flags = 0;
    std::uint32_t master_flags = 0;
    std::vector<LoadedActionAnim> anims{};
};

struct LoadedSkinDeformKey {
    std::string id{};
    std::int32_t time_point = 0;
    std::uint32_t flags = 0;
    float min_val = 0.0f;
    float max_val = 0.0f;
    std::string name{};
};

struct LoadedSkinFaceTexture {
    std::string material_name{};
    std::uint32_t color = 0;
    std::vector<std::string> hair_materials{};
    std::vector<std::uint32_t> gradient_colors{};
};

struct LoadedSkinVoice {
    std::int32_t type = 0;
    std::string sound_name{};
};

// mbFaceConstraint::readStream (0x140213ee0): value, type, count of (weight, key_index) pairs.
struct LoadedSkinConstraintPair {
    float weight = 0.0f;
    std::int32_t key_index = 0;
};

struct LoadedSkinConstraint {
    float value = 0.0f;
    std::int32_t type = 0;
    std::vector<LoadedSkinConstraintPair> pairs{};
};

struct LoadedSkin {
    std::string name{};
    std::uint32_t flags = 0;
    std::string body_mesh{};
    std::string left_calf_mesh{};
    std::string left_hand_mesh{};
    std::string head_mesh{};
    std::vector<LoadedSkinDeformKey> deform_keys{};
    std::vector<std::string> hair_meshes{};
    std::vector<std::string> beard_meshes{};
    std::vector<std::string> hair_materials{};
    std::vector<std::string> beard_materials{};
    std::vector<LoadedSkinFaceTexture> face_textures{};
    std::vector<LoadedSkinVoice> voices{};
    std::string skeleton_model{};
    float scale = 1.0f;
    std::int32_t blood_particle_1 = -1;
    std::int32_t blood_particle_2 = -1;
    std::vector<LoadedSkinConstraint> constraints{};
};

struct LoadedSoundSample {
    std::string file{};
    std::uint32_t flags = 0;
};

struct LoadedSoundRef {
    std::int32_t sample_no = -1;
    std::uint32_t flags = 0;
};

struct LoadedSound {
    std::string id{};
    std::uint32_t flags = 0;
    std::vector<LoadedSoundRef> refs{};
};

struct LoadedSoundBank {
    std::int32_t version = 0;
    std::vector<LoadedSoundSample> samples{};
    std::vector<LoadedSound> sounds{};
};

struct LoadedMusicTrack {
    std::string file{};
    std::uint32_t flags = 0;
    std::uint32_t continue_flags = 0;
};

struct LoadedParticleSystem {
    std::string id{};
    std::uint32_t flags = 0;
    std::string mesh_name{};
    std::array<float, 35> params{};
};

struct LoadedPostEffect {
    std::string id{};
    std::uint32_t flags = 0;
    std::int32_t tonemap_op = 0;
    std::array<float, 12> params{};
};

struct LoadedFloraInstance {
    std::string mesh_name{};
    std::string body_name{};
    std::string winter_mesh_name{};
    std::string winter_body_name{};
};

struct LoadedFloraKind {
    std::string name{};
    std::uint64_t flags = 0;
    std::vector<LoadedFloraInstance> instances{};
    float colony_radius = 0.0f;
    float colony_threshold = 0.0f;
};

struct LoadedGroundSpec {
    std::string id{};
    std::uint32_t flags = 0;
    std::string material_name{};
    float uv_scale = 1.0f;
    std::string multiset_name{};
    std::array<float, 3> color{};
};

struct LoadedSkybox {
    std::string id{};
    std::uint32_t flags = 0;
    float sun_heading = 0.0f;
    float sun_altitude = 0.0f;
    float flare_strength = 0.0f;
    std::string post_effect{};
    std::array<float, 3> sun_rgb{};
    std::array<float, 3> sky_rgb{};
    std::array<float, 3> ambient_rgb{};
    float fog_start = 0.0f;
    std::uint32_t fog_color = 0;
};

namespace detail {

[[nodiscard]] inline bool read_string_list(io::TokenReader& reader,
                                           std::int32_t count,
                                           std::vector<std::string>& out) {
    out.clear();
    out.reserve(reader.plausible_count(static_cast<std::size_t>(count), 2));
    for (std::int32_t i = 0; i < count; ++i) {
        std::string s;
        if (!reader.read_string(s)) {
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

} // namespace detail

// meshes.txt (0x140095ef0 / mbMeshManager::readStream 0x140121fb0 / mbMesh::readStream 0x140121ed0): zero header tokens.
[[nodiscard]] inline ModuleLoadError load_meshes(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedMesh>* out_meshes = nullptr,
    std::string_view source = "meshes.txt") {
    ModuleLoadError err{};
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "meshes", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.mesh_names.clear();
    world.mesh_names.reserve(reader.plausible_count(total, 16));
    if (out_meshes != nullptr) {
        out_meshes->clear();
        out_meshes->reserve(reader.plausible_count(total, 16));
    }

    for (std::size_t i = 0; i < total; ++i) {
        LoadedMesh m{};
        if (!reader.read_string(m.id) || !read_u32_token(reader, m.flags) ||
            !reader.read_string(m.resource_name)) {
            return make_load_error(source, "truncated mesh header", i, m.id);
        }
        for (float& v : m.transform) {
            if (!reader.read_f32(v)) {
                return make_load_error(source, "truncated mesh transform", i, m.id);
            }
        }
        world.mesh_names.push_back(m.id);
        if (out_meshes != nullptr) {
            out_meshes->push_back(std::move(m));
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_meshes(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedMesh>* out_meshes = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_meshes(reader, world, out_meshes, src);
        });
}

// actions.txt (0x140098680 / mbActionSet::readStream 0x140028dd0 / mbAction::readStream 0x140028cb0): zero header tokens.
[[nodiscard]] inline ModuleLoadError load_actions(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedAction>* out_actions = nullptr,
    std::string_view source = "actions.txt") {
    ModuleLoadError err{};
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "actions", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_actions != nullptr) {
        out_actions->clear();
        out_actions->reserve(reader.plausible_count(total, 16));
    }

    for (std::size_t i = 0; i < total; ++i) {
        LoadedAction act{};
        std::int32_t num_anims = 0;
        if (!reader.read_string(act.id) || !read_u32_token(reader, act.flags) ||
            !read_u32_token(reader, act.master_flags) ||
            !read_count_token(reader, num_anims, kMaxModuleRecords, source,
                              "action anims", err)) {
            if (err.ok) {
                return make_load_error(source, "truncated action header", i, act.id);
            }
            err.record_index = i;
            err.record_id = act.id;
            return err;
        }
        act.anims.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_anims), 16));
        for (std::int32_t a = 0; a < num_anims; ++a) {
            LoadedActionAnim anim{};
            if (!reader.read_f32(anim.duration) ||
                !reader.read_string(anim.anim_name) ||
                !read_i32_token(reader, anim.start_time) ||
                !read_i32_token(reader, anim.end_time) ||
                !read_u32_token(reader, anim.flags) ||
                !read_u32_token(reader, anim.sound_times) ||
                !reader.read_f32(anim.dx) || !reader.read_f32(anim.dy) ||
                !reader.read_f32(anim.dz) || !reader.read_f32(anim.ragdoll_delay)) {
                return make_load_error(source, "truncated action anim", i, act.id);
            }
            act.anims.push_back(std::move(anim));
        }
        if (out_actions != nullptr) {
            out_actions->push_back(std::move(act));
        }
    }
    world.misc.num_actions = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_actions(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedAction>* out_actions = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_actions(reader, world, out_actions, src);
        });
}

// skins.txt (0x140095120 / mbSkin::readStream 0x1402141c0): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_skins(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedSkin>* out_skins = nullptr,
    std::string_view source = "skins.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "skins", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_skins != nullptr) {
        out_skins->clear();
        out_skins->reserve(reader.plausible_count(total, 32));
    }

    for (std::size_t i = 0; i < total; ++i) {
        LoadedSkin skin{};
        std::int32_t num_deform_keys = 0;
        if (!reader.read_string(skin.name) ||
            !read_u32_token(reader, skin.flags) ||
            !reader.read_string(skin.body_mesh) ||
            !reader.read_string(skin.left_calf_mesh) ||
            !reader.read_string(skin.left_hand_mesh) ||
            !reader.read_string(skin.head_mesh) ||
            !read_count_token(reader, num_deform_keys, kMaxModuleRecords, source,
                              "skin deform keys", err)) {
            return err.ok ? make_load_error(source, "truncated skin header", i,
                                            skin.name)
                          : err;
        }
        skin.deform_keys.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_deform_keys), 8));
        for (std::int32_t d = 0; d < num_deform_keys; ++d) {
            LoadedSkinDeformKey k{};
            if (!reader.read_string(k.id) || !read_i32_token(reader, k.time_point) ||
                !read_u32_token(reader, k.flags) || !reader.read_f32(k.min_val) ||
                !reader.read_f32(k.max_val) || !reader.read_string(k.name)) {
                return make_load_error(source, "truncated skin deform key", i,
                                       skin.name);
            }
            underscores_to_spaces(k.name);
            skin.deform_keys.push_back(std::move(k));
        }

        std::int32_t n_hair = 0, n_beard = 0, n_hair_mat = 0, n_beard_mat = 0;
        if (!read_count_token(reader, n_hair, kMaxModuleRecords, source,
                              "skin hair meshes", err) ||
            !detail::read_string_list(reader, n_hair, skin.hair_meshes) ||
            !read_count_token(reader, n_beard, kMaxModuleRecords, source,
                              "skin beard meshes", err) ||
            !detail::read_string_list(reader, n_beard, skin.beard_meshes) ||
            !read_count_token(reader, n_hair_mat, kMaxModuleRecords, source,
                              "skin hair materials", err) ||
            !detail::read_string_list(reader, n_hair_mat, skin.hair_materials) ||
            !read_count_token(reader, n_beard_mat, kMaxModuleRecords, source,
                              "skin beard materials", err) ||
            !detail::read_string_list(reader, n_beard_mat, skin.beard_materials)) {
            return err.ok ? make_load_error(source, "truncated skin mesh/material list",
                                            i, skin.name)
                          : err;
        }

        std::int32_t num_faces = 0;
        if (!read_count_token(reader, num_faces, kMaxModuleRecords, source,
                              "skin face textures", err)) {
            return err;
        }
        skin.face_textures.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_faces), 8));
        for (std::int32_t f = 0; f < num_faces; ++f) {
            LoadedSkinFaceTexture ft{};
            std::int32_t n_fh = 0;
            std::int32_t n_grad = 0;
            if (!reader.read_string(ft.material_name) ||
                !read_wrapping_u32_token(reader, ft.color) ||
                !read_count_token(reader, n_fh, kMaxModuleRecords, source,
                                  "face hair materials", err) ||
                !read_count_token(reader, n_grad, kMaxModuleRecords, source,
                                  "face gradient colors", err) ||
                !detail::read_string_list(reader, n_fh, ft.hair_materials)) {
                return err.ok ? make_load_error(source, "truncated skin face texture",
                                                i, skin.name)
                              : err;
            }
            ft.gradient_colors.resize(static_cast<std::size_t>(n_grad));
            for (std::int32_t g = 0; g < n_grad; ++g) {
                if (!read_wrapping_u32_token(
                        reader, ft.gradient_colors[static_cast<std::size_t>(g)])) {
                    return make_load_error(source, "truncated skin gradient color",
                                           i, skin.name);
                }
            }
            skin.face_textures.push_back(std::move(ft));
        }

        std::int32_t num_voices = 0;
        if (!read_count_token(reader, num_voices, kMaxModuleRecords, source,
                              "skin voices", err)) {
            return err;
        }
        skin.voices.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_voices), 4));
        for (std::int32_t v = 0; v < num_voices; ++v) {
            LoadedSkinVoice voice{};
            if (!read_i32_token(reader, voice.type) ||
                !reader.read_string(voice.sound_name)) {
                return make_load_error(source, "truncated skin voice", i,
                                       skin.name);
            }
            skin.voices.push_back(std::move(voice));
        }

        std::int32_t num_constraints = 0;
        if (!reader.read_string(skin.skeleton_model) ||
            !reader.read_f32(skin.scale) ||
            !read_i32_token(reader, skin.blood_particle_1) ||
            !read_i32_token(reader, skin.blood_particle_2) ||
            !read_count_token(reader, num_constraints, kMaxModuleRecords, source,
                              "skin constraints", err)) {
            return err.ok ? make_load_error(source, "truncated skin skeleton/blood",
                                            i, skin.name)
                          : err;
        }
        skin.constraints.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_constraints), 6));
        for (std::int32_t c = 0; c < num_constraints; ++c) {
            LoadedSkinConstraint con{};
            std::int32_t w_count = 0;
            if (!reader.read_f32(con.value) || !read_i32_token(reader, con.type) ||
                !read_count_token(reader, w_count, kMaxModuleRecords, source,
                                  "skin constraint weights", err)) {
                return err.ok ? make_load_error(source, "truncated skin constraint",
                                                i, skin.name)
                              : err;
            }
            con.pairs.resize(static_cast<std::size_t>(w_count));
            for (std::int32_t w = 0; w < w_count; ++w) {
                auto& pair = con.pairs[static_cast<std::size_t>(w)];
                if (!reader.read_f32(pair.weight) ||
                    !read_i32_token(reader, pair.key_index)) {
                    return make_load_error(source,
                                           "truncated skin constraint weight pair",
                                           i, skin.name);
                }
            }
            skin.constraints.push_back(std::move(con));
        }

        if (out_skins != nullptr) {
            out_skins->push_back(std::move(skin));
        }
    }
    world.num_skins = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_skins(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedSkin>* out_skins = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_skins(reader, world, out_skins, src);
        });
}

// sounds.txt (0x1400949d0 / mbSoundManager::readStream 0x1403330a0): skips 2 strings + `version`.
[[nodiscard]] inline ModuleLoadError load_sounds(
    io::TokenReader& reader,
    WseWorldState& world,
    LoadedSoundBank* out_bank = nullptr,
    std::string_view source = "sounds.txt") {
    ModuleLoadError err{};
    std::int32_t version = 0;
    if (!read_versioned_header(reader, version, source, err)) {
        return err;
    }

    std::int32_t num_samples = 0;
    if (!read_count_token(reader, num_samples, kMaxModuleRecords, source,
                          "sound samples", err)) {
        return err;
    }
    LoadedSoundBank bank{};
    bank.version = version;
    bank.samples.reserve(
        reader.plausible_count(static_cast<std::size_t>(num_samples), 4));
    for (std::int32_t i = 0; i < num_samples; ++i) {
        LoadedSoundSample sample{};
        if (!reader.read_string(sample.file) ||
            !read_u32_token(reader, sample.flags)) {
            return make_load_error(source, "truncated sound sample",
                                   static_cast<std::size_t>(i), sample.file);
        }
        if (out_bank != nullptr) {
            bank.samples.push_back(std::move(sample));
        }
    }

    std::int32_t num_sounds = 0;
    if (!read_count_token(reader, num_sounds, kMaxModuleRecords, source,
                          "sounds", err)) {
        return err;
    }
    if (out_bank != nullptr) {
        bank.sounds.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_sounds), 6));
    }
    for (std::int32_t i = 0; i < num_sounds; ++i) {
        LoadedSound snd{};
        std::int32_t num_refs = 0;
        if (!reader.read_string(snd.id) || !read_u32_token(reader, snd.flags) ||
            !read_count_token(reader, num_refs, kMaxModuleRecords, source,
                              "sound sample refs", err)) {
            return err.ok ? make_load_error(source, "truncated sound entry",
                                            static_cast<std::size_t>(i), snd.id)
                          : err;
        }
        snd.refs.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_refs), 2));
        for (std::int32_t r = 0; r < num_refs; ++r) {
            LoadedSoundRef ref{};
            if (!read_i32_token(reader, ref.sample_no)) {
                return make_load_error(source, "truncated sound sample ref",
                                       static_cast<std::size_t>(i), snd.id);
            }
            if (version >= 3 && !read_u32_token(reader, ref.flags)) {
                return make_load_error(source, "truncated sound ref flags",
                                       static_cast<std::size_t>(i), snd.id);
            }
            if (out_bank != nullptr) {
                snd.refs.push_back(ref);
            }
        }
        if (out_bank != nullptr) {
            bank.sounds.push_back(std::move(snd));
        }
    }

    world.m5.num_sounds = num_sounds;
    if (out_bank != nullptr) {
        *out_bank = std::move(bank);
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_sounds(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    LoadedSoundBank* out_bank = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_sounds(reader, world, out_bank, src);
        });
}

// music.txt (mbMusicManager::readStream 0x140290c30 / mbMusicTrack::readStream 0x140290870): zero header tokens.
[[nodiscard]] inline ModuleLoadError load_music(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedMusicTrack>* out_tracks = nullptr,
    std::string_view source = "music.txt") {
    ModuleLoadError err{};
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "music tracks", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_tracks != nullptr) {
        out_tracks->clear();
        out_tracks->reserve(reader.plausible_count(total, 6));
    }
    for (std::size_t i = 0; i < total; ++i) {
        LoadedMusicTrack track{};
        if (!reader.read_string(track.file) ||
            !read_u32_token(reader, track.flags) ||
            !read_u32_token(reader, track.continue_flags)) {
            return make_load_error(source, "truncated music track", i, track.file);
        }
        if (out_tracks != nullptr) {
            out_tracks->push_back(std::move(track));
        }
    }
    world.misc.num_music_tracks = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_music(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedMusicTrack>* out_tracks = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_music(reader, world, out_tracks, src);
        });
}

// particle_systems.txt (0x140094d80 / mbParticleSystemManager::readStream 0x140208190 / mbParticleSystem::readStream 0x140207ed0):
// 3 header tokens + 30 floats per entry.
[[nodiscard]] inline ModuleLoadError load_particle_systems(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedParticleSystem>* out_systems = nullptr,
    std::string_view source = "particle_systems.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "particle systems", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_systems != nullptr) {
        out_systems->clear();
        out_systems->reserve(reader.plausible_count(total, 64));
    }
    for (std::size_t i = 0; i < total; ++i) {
        LoadedParticleSystem ps{};
        if (!reader.read_string(ps.id) || !read_u32_token(reader, ps.flags) ||
            !reader.read_string(ps.mesh_name)) {
            return make_load_error(source, "truncated particle system", i, ps.id);
        }
        for (float& v : ps.params) {
            if (!reader.read_f32(v)) {
                return make_load_error(source,
                                       "truncated particle system parameter", i,
                                       ps.id);
            }
        }
        if (out_systems != nullptr) {
            out_systems->push_back(std::move(ps));
        }
    }
    world.num_particle_systems = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_particle_systems(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedParticleSystem>* out_systems = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_particle_systems(reader, world, out_systems, src);
        });
}

// postfx.txt (0x140096000 / mbPostEffect::readStream 0x140208d70): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_post_effects(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedPostEffect>* out_effects = nullptr,
    std::string_view source = "postfx.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "post effects",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_effects != nullptr) {
        out_effects->clear();
        out_effects->reserve(reader.plausible_count(total, 32));
    }
    for (std::size_t i = 0; i < total; ++i) {
        LoadedPostEffect pfx{};
        if (!reader.read_string(pfx.id) || !read_u32_token(reader, pfx.flags) ||
            !read_i32_token(reader, pfx.tonemap_op)) {
            return make_load_error(source, "truncated post effect", i, pfx.id);
        }
        for (float& v : pfx.params) {
            if (!reader.read_f32(v)) {
                return make_load_error(source, "truncated post effect parameter",
                                       i, pfx.id);
            }
        }
        if (out_effects != nullptr) {
            out_effects->push_back(std::move(pfx));
        }
    }
    world.misc.num_post_effects = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_post_effects(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedPostEffect>* out_effects = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_post_effects(reader, world, out_effects, src);
        });
}

// Optional Data/flora_kinds.txt (0x140094160 / mbFloraKind::readStream): 0 header tokens.
[[nodiscard]] inline ModuleLoadError load_flora_kinds(
    io::TokenReader& reader,
    std::vector<LoadedFloraKind>& out,
    std::string_view source = "Data/flora_kinds.txt") {
    ModuleLoadError err{};
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "flora kinds",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    out.clear();
    out.reserve(reader.plausible_count(total, 8));
    for (std::size_t i = 0; i < total; ++i) {
        LoadedFloraKind flora{};
        std::int32_t num_instances = 0;
        if (!reader.read_string(flora.name) || !reader.read_u64(flora.flags) ||
            !read_count_token(reader, num_instances, kMaxModuleRecords, source,
                              "flora instances", err)) {
            return err.ok ? make_load_error(source, "truncated flora kind", i,
                                            flora.name)
                          : err;
        }
        flora.instances.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_instances), 4));
        const bool has_winter = (flora.flags & (1ULL << 22U)) != 0ULL;
        const bool has_colony = (flora.flags & (1ULL << 26U)) != 0ULL;
        for (std::int32_t n = 0; n < num_instances; ++n) {
            LoadedFloraInstance inst{};
            if (!reader.read_string(inst.mesh_name) ||
                !reader.read_string(inst.body_name)) {
                return make_load_error(source, "truncated flora instance", i,
                                       flora.name);
            }
            if (has_winter && (!reader.read_string(inst.winter_mesh_name) ||
                               !reader.read_string(inst.winter_body_name))) {
                return make_load_error(source, "truncated winter flora instance",
                                       i, flora.name);
            }
            flora.instances.push_back(std::move(inst));
        }
        if (has_colony && (!reader.read_f32(flora.colony_radius) ||
                           !reader.read_f32(flora.colony_threshold))) {
            return make_load_error(source, "truncated flora colony", i,
                                   flora.name);
        }
        out.push_back(std::move(flora));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_flora_kinds(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedFloraKind>& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_flora_kinds(reader, out, src);
        });
}

// Optional Data/ground_specs.txt (0x140094410): fixed 11 entries.
[[nodiscard]] inline ModuleLoadError load_ground_specs(
    io::TokenReader& reader,
    std::vector<LoadedGroundSpec>& out,
    std::string_view source = "Data/ground_specs.txt") {
    constexpr std::size_t kNumGroundSpecs = 11U;
    out.clear();
    out.resize(kNumGroundSpecs);
    for (std::size_t i = 0; i < kNumGroundSpecs; ++i) {
        auto& g = out[i];
        if (!reader.read_string(g.id) || !read_u32_token(reader, g.flags) ||
            !reader.read_string(g.material_name) ||
            !reader.read_f32(g.uv_scale) ||
            !reader.read_string(g.multiset_name)) {
            return make_load_error(source, "truncated ground spec", i, g.id);
        }
        if ((g.flags & 4U) != 0U &&
            (!reader.read_f32(g.color[0]) || !reader.read_f32(g.color[1]) ||
             !reader.read_f32(g.color[2]))) {
            return make_load_error(source, "truncated ground spec color", i, g.id);
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_ground_specs(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedGroundSpec>& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_ground_specs(reader, out, src);
        });
}

// Optional Data/skyboxes.txt (0x140094560 / mbSkybox::readStream 0x140218240): zero header tokens.
[[nodiscard]] inline ModuleLoadError load_skyboxes(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedSkybox>* out_skyboxes = nullptr,
    std::string_view source = "Data/skyboxes.txt") {
    ModuleLoadError err{};
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "skyboxes",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_skyboxes != nullptr) {
        out_skyboxes->clear();
        out_skyboxes->reserve(reader.plausible_count(total, 32));
    }
    for (std::size_t i = 0; i < total; ++i) {
        LoadedSkybox sky{};
        if (!reader.read_string(sky.id) || !read_u32_token(reader, sky.flags) ||
            !reader.read_f32(sky.sun_heading) ||
            !reader.read_f32(sky.sun_altitude) ||
            !reader.read_f32(sky.flare_strength) ||
            !reader.read_string(sky.post_effect) ||
            !reader.read_f32(sky.sun_rgb[0]) || !reader.read_f32(sky.sun_rgb[1]) ||
            !reader.read_f32(sky.sun_rgb[2]) || !reader.read_f32(sky.sky_rgb[0]) ||
            !reader.read_f32(sky.sky_rgb[1]) || !reader.read_f32(sky.sky_rgb[2]) ||
            !reader.read_f32(sky.ambient_rgb[0]) ||
            !reader.read_f32(sky.ambient_rgb[1]) ||
            !reader.read_f32(sky.ambient_rgb[2]) ||
            !reader.read_f32(sky.fog_start) ||
            !read_wrapping_u32_token(reader, sky.fog_color)) {
            return make_load_error(source, "truncated skybox", i, sky.id);
        }
        if (i < world.m5.skybox_fog_colors.size()) {
            world.m5.skybox_fog_colors[i] = sky.fog_color;
        }
        if (out_skyboxes != nullptr) {
            out_skyboxes->push_back(std::move(sky));
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_skyboxes(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedSkybox>* out_skyboxes = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_skyboxes(reader, world, out_skyboxes, src);
        });
}

} // namespace wse::game
