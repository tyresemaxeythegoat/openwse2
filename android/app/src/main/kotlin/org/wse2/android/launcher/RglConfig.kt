package org.wse2.android.launcher

import java.io.File
import java.util.LinkedHashMap
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Full typed model of Mount & Blade: Warband's `rgl_config.txt`.
 *
 * Covers every property present in a retail `rgl_config.txt` and preserves any additional
 * user-defined keys verbatim in [extraEntries] when saving.
 */
data class RglConfigState(
    // Video & rendering
    val textureDetail: Int = 100,
    val renderBufferSize: Int = 15,
    val maxFramerate: Int = 60,
    val startWindowed: Boolean = false,
    val usePixelShaders: Boolean = true,
    val useVertexShaders: Boolean = true,
    val fakeReflections: Boolean = true,
    val showFramerate: Boolean = false,
    val useOnDemandTextures: Boolean = true,
    val useOnDemandTexturesMt: Boolean = true,
    val displayWidth: Int = 0,
    val displayHeight: Int = 0,
    val displayBits: Int = 32,
    val antialiasing: Int = 1,
    val sampleQuality: Int = 0,
    val alpha2Coverage: Boolean = false,
    val forceVsync: Boolean = true,
    val shaderQuality: Int = 2,
    val postfxDof: Boolean = false,
    val postfxHdr: Int = 0,
    val postfxAutoexp: Boolean = false,
    val useInstancing: Boolean = false,
    val anisotropicFiltering: Boolean = true,
    val adapterFormat: Int = 0,
    val gamma: Float = 2.2f,
    val enableAspectRatioControl: Boolean = true,
    val autoGfxQuality: Boolean = false,

    // Shadows, flora & world
    val shadowmapQuality: Int = 1,
    val enableLighting: Boolean = true,
    val enableParticles: Boolean = true,
    val enableBlood: Boolean = true,
    val bloodStains: Int = 1,
    val enableCharacterShadows: Boolean = true,
    val enableAccurateShadows: Boolean = true,
    val enableEnvironmentShadows: Boolean = true,
    val realisticShadowsOnPlants: Int = 2,
    val numberOfCorpses: Int = 3,
    val numberOfRagdolls: Int = 3,
    val grassDensity: Int = 50,
    val floraDegradeDistance: Float = 100.0f,
    val floraLodDetail: Int = 0,
    val characterDetail: Float = 0.5f,
    val characterShadowDetail: Float = 0.5f,

    // Gameplay, combat & HUD
    val battleSize: Float = 0.3422f,
    val combatSpeed: Int = 2,
    val combatDifficulty: Int = 0,
    val friendCombatDifficulty: Int = 0,
    val reduceCombatAi: Int = 2,
    val reduceCampaignAi: Int = 2,
    val attackDirectionControl: Int = 2,
    val defendDirectionControl: Int = 0,
    val lanceControl: Int = 0,
    val turnCameraWithHorseInFirstPerson: Int = 2,
    val displayLabels: Boolean = true,
    val displayTargetingReticule: Boolean = true,
    val displayAttackDirection: Boolean = false,
    val verboseDamage: Boolean = true,
    val verboseShotDifficulty: Boolean = true,
    val verboseCasualties: Boolean = true,
    val verboseExperience: Boolean = true,
    val realisticHeadshots: Boolean = false,

    // Audio & input
    val musicVolume: Float = 0.4f,
    val soundVolume: Float = 1.0f,
    val disableMusic: Boolean = false,
    val disableSound: Boolean = false,
    val disableFrequencyVariation: Boolean = false,
    val useWinmmAudio: Boolean = false,
    val mouseSensitivity: Float = 0.5f,
    val invertMouse: Boolean = false,
    val controlMouseMovementYScale: Float = 1.5f,
    val enableGamepadVibration: Boolean = true,

    // System, engine & network
    val firstTime: Boolean = false,
    val cheatMode: Boolean = false,
    val enableEditMode: Boolean = false,
    val forceSingleThreading: Boolean = false,
    val debugMode: Boolean = false,
    val useSecureConnection: Boolean = false,
    val maxNumberOfConnections: Int = 16,
    val lookForServerOnThisMachine: Boolean = false,
    val enableVersionCheck: Boolean = true,

    // Unrecognized properties preserved verbatim on round-trip
    val extraEntries: Map<String, String> = emptyMap()
) {
    /** Approximate soldier count using Warband's standard 30..150 scale. */
    val battleSizeTroops: Int
        get() = (30 + (battleSize.coerceIn(0f, 1f) * 120f).roundToInt()).coerceIn(30, 150)

    fun withQualityPreset(presetKey: String): RglConfigState = when (presetKey) {
        "low" -> copy(
            textureDetail = 50,
            shaderQuality = 0,
            antialiasing = 0,
            anisotropicFiltering = false,
            shadowmapQuality = 0,
            enableCharacterShadows = false,
            enableAccurateShadows = false,
            enableEnvironmentShadows = false,
            realisticShadowsOnPlants = 0,
            postfxHdr = 0,
            postfxDof = false,
            postfxAutoexp = false,
            grassDensity = 20,
            floraLodDetail = 2,
            floraDegradeDistance = 60.0f,
            characterDetail = 0.35f,
            characterShadowDetail = 0.25f,
            numberOfCorpses = 1,
            numberOfRagdolls = 1,
            battleSize = 0.25f
        )
        "medium" -> copy(
            textureDetail = 80,
            shaderQuality = 1,
            antialiasing = 1,
            anisotropicFiltering = true,
            shadowmapQuality = 1,
            enableCharacterShadows = true,
            enableAccurateShadows = false,
            enableEnvironmentShadows = true,
            realisticShadowsOnPlants = 1,
            postfxHdr = 0,
            postfxDof = false,
            postfxAutoexp = false,
            grassDensity = 50,
            floraLodDetail = 1,
            floraDegradeDistance = 100.0f,
            characterDetail = 0.5f,
            characterShadowDetail = 0.5f,
            numberOfCorpses = 3,
            numberOfRagdolls = 2,
            battleSize = 0.5f
        )
        "high" -> copy(
            textureDetail = 100,
            shaderQuality = 2,
            antialiasing = 2,
            anisotropicFiltering = true,
            shadowmapQuality = 2,
            enableCharacterShadows = true,
            enableAccurateShadows = true,
            enableEnvironmentShadows = true,
            realisticShadowsOnPlants = 2,
            postfxHdr = 1,
            postfxDof = false,
            postfxAutoexp = true,
            grassDensity = 80,
            floraLodDetail = 0,
            floraDegradeDistance = 150.0f,
            characterDetail = 1.0f,
            characterShadowDetail = 1.0f,
            numberOfCorpses = 5,
            numberOfRagdolls = 4,
            battleSize = 1.0f
        )
        else -> this
    }
}

object RglConfigOptions {
    val SHADER_QUALITY_LABELS = listOf("Low", "Medium", "High")
    val SHADOWMAP_QUALITY_LABELS = listOf("Low", "High", "Ultra")
    val ANTIALIASING_VALUES = listOf(0, 1, 2, 4, 8)
    val ANTIALIASING_LABELS = listOf("Off", "1x", "2x", "4x", "8x")
    val POSTFX_HDR_LABELS = listOf("Off", "Low", "High")
    val ADAPTER_FORMAT_LABELS = listOf("Default", "16-bit", "32-bit")
    val DISPLAY_BITS_VALUES = listOf(16, 32)
    val DISPLAY_BITS_LABELS = listOf("16-bit", "32-bit")
    val RESOLUTION_PRESETS = listOf(
        0 to 0,
        1280 to 720,
        1600 to 900,
        1920 to 1080,
        2560 to 1440
    )
    val RESOLUTION_LABELS = listOf("Auto", "720p", "900p", "1080p", "1440p")

    val PLANT_SHADOW_LABELS = listOf("None", "Quick", "Full")
    val FLORA_LOD_LABELS = listOf("High", "Medium", "Low")
    val BLOOD_STAINS_LABELS = listOf("Off", "Near", "All")
    val CORPSES_LABELS = listOf("None", "5", "10", "20", "40", "100", "Unlimited")
    val RAGDOLLS_LABELS = listOf("None", "3", "5", "10", "Unlimited")

    val COMBAT_SPEED_LABELS = listOf("Slowest", "Slower", "Normal", "Faster", "Fastest")
    val DAMAGE_DIFFICULTY_LABELS = listOf("1/4", "1/2", "Normal")
    val AI_LEVEL_LABELS = listOf("Good", "Average", "Poor")
    val ATTACK_CONTROL_LABELS = listOf(
        "Mouse movement",
        "Inverse mouse",
        "Relative to enemy",
        "Movement keys"
    )
    val DEFEND_CONTROL_LABELS = listOf(
        "Automatic",
        "Mouse movement",
        "Inverse mouse",
        "Movement keys"
    )
    val LANCE_CONTROL_LABELS = listOf("Automatic", "Manual (Easy)", "Manual (Hard)")
    val HORSE_CAMERA_LABELS = listOf("Never", "On horse", "Always")
}

object RglConfigCodec {
    private fun parseBool(raw: String?, default: Boolean): Boolean {
        val v = raw?.trim() ?: return default
        return when (v.lowercase(Locale.US)) {
            "1", "true", "yes" -> true
            "0", "false", "no" -> false
            else -> v.toIntOrNull()?.let { it != 0 } ?: default
        }
    }

    private fun parseInt(raw: String?, default: Int, min: Int, max: Int): Int {
        val v = raw?.trim()?.toIntOrNull() ?: return default
        return v.coerceIn(min, max)
    }

    private fun parseFloat(raw: String?, default: Float, min: Float, max: Float): Float {
        val v = raw?.trim()?.toFloatOrNull() ?: return default
        if (!v.isFinite()) return default
        return v.coerceIn(min, max)
    }

    private fun b(v: Boolean): String = if (v) "1" else "0"

    private fun f(v: Float): String = String.format(Locale.US, "%.4f", v)

    fun parse(text: String): RglConfigState {
        val defaults = RglConfigState()
        val parsed = LinkedHashMap<String, String>()
        for (rawLine in text.lineSequence()) {
            val line = rawLine.trim()
            if (line.isEmpty() || line.startsWith("#") || line.startsWith("//") || line.startsWith(";")) {
                continue
            }
            val eq = line.indexOf('=')
            if (eq <= 0) continue
            val key = line.substring(0, eq).trim()
            val value = line.substring(eq + 1).trim()
            if (key.isNotEmpty()) {
                parsed[key] = value
            }
        }

        val extras = LinkedHashMap<String, String>()
        for ((k, v) in parsed) {
            if (k !in KNOWN_KEYS) {
                extras[k] = v
            }
        }

        return RglConfigState(
            firstTime = parseBool(parsed["first_time"], defaults.firstTime),
            textureDetail = parseInt(parsed["texture_detail"], defaults.textureDetail, 10, 100),
            renderBufferSize = parseInt(parsed["render_buffer_size"], defaults.renderBufferSize, 15, 512),
            maxFramerate = parseInt(parsed["max_framerate"], defaults.maxFramerate, 30, 240),
            startWindowed = parseBool(parsed["start_windowed"], defaults.startWindowed),
            usePixelShaders = parseBool(parsed["use_pixel_shaders"], defaults.usePixelShaders),
            useVertexShaders = parseBool(parsed["use_vertex_shaders"], defaults.useVertexShaders),
            fakeReflections = parseBool(parsed["fake_reflections"], defaults.fakeReflections),
            showFramerate = parseBool(parsed["show_framerate"], defaults.showFramerate),
            useOnDemandTextures = parseBool(parsed["use_ondemand_textures_"], defaults.useOnDemandTextures),
            useOnDemandTexturesMt = parseBool(parsed["use_ondemand_textures_mt"], defaults.useOnDemandTexturesMt),
            disableMusic = parseBool(parsed["disable_music"], defaults.disableMusic),
            disableSound = parseBool(parsed["disable_sound"], defaults.disableSound),
            disableFrequencyVariation = parseBool(parsed["disable_frequency_variation"], defaults.disableFrequencyVariation),
            cheatMode = parseBool(parsed["cheat_mode"], defaults.cheatMode),
            enableBlood = parseBool(parsed["enable_blood"], defaults.enableBlood),
            enableEditMode = parseBool(parsed["enable_edit_mode"], defaults.enableEditMode),
            forceSingleThreading = parseBool(parsed["force_single_threading"], defaults.forceSingleThreading),
            debugMode = parseBool(parsed["debug_mode"], defaults.debugMode),
            displayWidth = parseInt(parsed["display_width"], defaults.displayWidth, 0, 7680),
            displayHeight = parseInt(parsed["display_height"], defaults.displayHeight, 0, 4320),
            displayBits = parseInt(parsed["display_bits"], defaults.displayBits, 16, 32),
            antialiasing = parseInt(parsed["antialiasing"], defaults.antialiasing, 0, 8),
            sampleQuality = parseInt(parsed["sample_quality"], defaults.sampleQuality, 0, 4),
            alpha2Coverage = parseBool(parsed["alpha2coverage"], defaults.alpha2Coverage),
            forceVsync = parseBool(parsed["force_vsync"], defaults.forceVsync),
            shadowmapQuality = parseInt(parsed["shadowmap_quality"], defaults.shadowmapQuality, 0, 2),
            shaderQuality = parseInt(parsed["shader_quality"], defaults.shaderQuality, 0, 2),
            postfxDof = parseBool(parsed["postfx_dof"], defaults.postfxDof),
            postfxHdr = parseInt(parsed["postfx_hdr"], defaults.postfxHdr, 0, 2),
            postfxAutoexp = parseBool(parsed["postfx_autoexp"], defaults.postfxAutoexp),
            floraDegradeDistance = parseFloat(parsed["flora_degrade_distance"], defaults.floraDegradeDistance, 40f, 200f),
            floraLodDetail = parseInt(parsed["flora_lod_detail"], defaults.floraLodDetail, 0, 2),
            useInstancing = parseBool(parsed["use_instancing"], defaults.useInstancing),
            useSecureConnection = parseBool(parsed["use_secure_connection"], defaults.useSecureConnection),
            maxNumberOfConnections = parseInt(parsed["max_number_of_connections"], defaults.maxNumberOfConnections, 2, 200),
            lookForServerOnThisMachine = parseBool(parsed["look_for_server_on_this_machine"], defaults.lookForServerOnThisMachine),
            musicVolume = parseFloat(parsed["music_volume"], defaults.musicVolume, 0f, 1f),
            soundVolume = parseFloat(parsed["sound_volume"], defaults.soundVolume, 0f, 1f),
            mouseSensitivity = parseFloat(parsed["mouse_sensitivity"], defaults.mouseSensitivity, 0.1f, 2.0f),
            invertMouse = parseBool(parsed["invert_mouse"], defaults.invertMouse),
            enableLighting = parseBool(parsed["enable_lighting"], defaults.enableLighting),
            enableParticles = parseBool(parsed["enable_particles"], defaults.enableParticles),
            enableCharacterShadows = parseBool(parsed["enable_character_shadows"], defaults.enableCharacterShadows),
            enableAccurateShadows = parseBool(parsed["enable_accurate_shadows"], defaults.enableAccurateShadows),
            numberOfCorpses = parseInt(parsed["number_of_corpses"], defaults.numberOfCorpses, 0, 6),
            grassDensity = parseInt(parsed["grass_density"], defaults.grassDensity, 0, 100),
            combatSpeed = parseInt(parsed["combat_speed"], defaults.combatSpeed, 0, 4),
            friendCombatDifficulty = parseInt(parsed["friend_combat_difficulty"], defaults.friendCombatDifficulty, 0, 2),
            adapterFormat = parseInt(parsed["adapter_format"], defaults.adapterFormat, 0, 2),
            reduceCombatAi = parseInt(parsed["reduce_combat_ai"], defaults.reduceCombatAi, 0, 2),
            reduceCampaignAi = parseInt(parsed["reduce_campaign_ai"], defaults.reduceCampaignAi, 0, 2),
            combatDifficulty = parseInt(parsed["combat_difficulty"], defaults.combatDifficulty, 0, 2),
            displayLabels = parseBool(parsed["display_labels"], defaults.displayLabels),
            displayTargetingReticule = parseBool(parsed["display_targeting_reticule"], defaults.displayTargetingReticule),
            displayAttackDirection = parseBool(parsed["display_attack_direction"], defaults.displayAttackDirection),
            turnCameraWithHorseInFirstPerson = parseInt(
                parsed["turn_camera_with_horse_in_first_person"],
                defaults.turnCameraWithHorseInFirstPerson,
                0,
                2
            ),
            verboseDamage = parseBool(parsed["verbose_damage"], defaults.verboseDamage),
            verboseShotDifficulty = parseBool(parsed["verbose_shot_difficulty"], defaults.verboseShotDifficulty),
            battleSize = parseFloat(parsed["battle_size"], defaults.battleSize, 0f, 1f),
            attackDirectionControl = parseInt(parsed["attack_direction_control"], defaults.attackDirectionControl, 0, 3),
            defendDirectionControl = parseInt(parsed["defend_direction_control"], defaults.defendDirectionControl, 0, 3),
            lanceControl = parseInt(parsed["lance_control"], defaults.lanceControl, 0, 2),
            anisotropicFiltering = parseBool(parsed["anisotropic_filtering"], defaults.anisotropicFiltering),
            enableEnvironmentShadows = parseBool(parsed["enable_environment_shadows"], defaults.enableEnvironmentShadows),
            verboseCasualties = parseBool(parsed["verbose_casualties"], defaults.verboseCasualties),
            verboseExperience = parseBool(parsed["verbose_experience"], defaults.verboseExperience),
            realisticShadowsOnPlants = parseInt(parsed["realistic_shadows_on_plants"], defaults.realisticShadowsOnPlants, 0, 2),
            numberOfRagdolls = parseInt(parsed["number_of_ragdolls"], defaults.numberOfRagdolls, 0, 4),
            gamma = parseFloat(parsed["gamma"], defaults.gamma, 1.0f, 3.0f),
            characterDetail = parseFloat(parsed["character_detail"], defaults.characterDetail, 0f, 1f),
            characterShadowDetail = parseFloat(parsed["character_shadow_detail"], defaults.characterShadowDetail, 0f, 1f),
            controlMouseMovementYScale = parseFloat(
                parsed["control_mouse_movement_y_scale"],
                defaults.controlMouseMovementYScale,
                0.5f,
                3.0f
            ),
            bloodStains = parseInt(parsed["blood_stains"], defaults.bloodStains, 0, 2),
            useWinmmAudio = parseBool(parsed["use_winmm_audio"], defaults.useWinmmAudio),
            enableVersionCheck = parseBool(parsed["enable_version_check"], defaults.enableVersionCheck),
            enableAspectRatioControl = parseBool(parsed["enable_aspect_ratio_control"], defaults.enableAspectRatioControl),
            realisticHeadshots = parseBool(parsed["realistic_headshots"], defaults.realisticHeadshots),
            autoGfxQuality = parseBool(parsed["auto_gfx_quality"], defaults.autoGfxQuality),
            enableGamepadVibration = parseBool(parsed["enable_gamepad_vibration"], defaults.enableGamepadVibration),
            extraEntries = extras
        )
    }

    fun serialize(state: RglConfigState): String {
        val entries = listOf(
            "first_time" to b(state.firstTime),
            "texture_detail" to state.textureDetail.toString(),
            "render_buffer_size" to state.renderBufferSize.toString(),
            "max_framerate" to state.maxFramerate.toString(),
            "start_windowed" to b(state.startWindowed),
            "use_pixel_shaders" to b(state.usePixelShaders),
            "use_vertex_shaders" to b(state.useVertexShaders),
            "fake_reflections" to b(state.fakeReflections),
            "show_framerate" to b(state.showFramerate),
            "use_ondemand_textures_" to b(state.useOnDemandTextures),
            "use_ondemand_textures_mt" to b(state.useOnDemandTexturesMt),
            "disable_music" to b(state.disableMusic),
            "disable_sound" to b(state.disableSound),
            "disable_frequency_variation" to b(state.disableFrequencyVariation),
            "cheat_mode" to b(state.cheatMode),
            "enable_blood" to b(state.enableBlood),
            "enable_edit_mode" to b(state.enableEditMode),
            "force_single_threading" to b(state.forceSingleThreading),
            "debug_mode" to b(state.debugMode),
            "display_width" to state.displayWidth.toString(),
            "display_height" to state.displayHeight.toString(),
            "display_bits" to state.displayBits.toString(),
            "antialiasing" to state.antialiasing.toString(),
            "sample_quality" to state.sampleQuality.toString(),
            "alpha2coverage" to b(state.alpha2Coverage),
            "force_vsync" to b(state.forceVsync),
            "shadowmap_quality" to state.shadowmapQuality.toString(),
            "shader_quality" to state.shaderQuality.toString(),
            "postfx_dof" to b(state.postfxDof),
            "postfx_hdr" to state.postfxHdr.toString(),
            "postfx_autoexp" to b(state.postfxAutoexp),
            "flora_degrade_distance" to f(state.floraDegradeDistance),
            "flora_lod_detail" to state.floraLodDetail.toString(),
            "use_instancing" to b(state.useInstancing),
            "use_secure_connection" to b(state.useSecureConnection),
            "max_number_of_connections" to state.maxNumberOfConnections.toString(),
            "look_for_server_on_this_machine" to b(state.lookForServerOnThisMachine),
            "music_volume" to f(state.musicVolume),
            "sound_volume" to f(state.soundVolume),
            "mouse_sensitivity" to f(state.mouseSensitivity),
            "invert_mouse" to b(state.invertMouse),
            "enable_lighting" to b(state.enableLighting),
            "enable_particles" to b(state.enableParticles),
            "enable_character_shadows" to b(state.enableCharacterShadows),
            "enable_accurate_shadows" to b(state.enableAccurateShadows),
            "number_of_corpses" to state.numberOfCorpses.toString(),
            "grass_density" to state.grassDensity.toString(),
            "combat_speed" to state.combatSpeed.toString(),
            "friend_combat_difficulty" to state.friendCombatDifficulty.toString(),
            "adapter_format" to state.adapterFormat.toString(),
            "reduce_combat_ai" to state.reduceCombatAi.toString(),
            "reduce_campaign_ai" to state.reduceCampaignAi.toString(),
            "combat_difficulty" to state.combatDifficulty.toString(),
            "display_labels" to b(state.displayLabels),
            "display_targeting_reticule" to b(state.displayTargetingReticule),
            "display_attack_direction" to b(state.displayAttackDirection),
            "turn_camera_with_horse_in_first_person" to state.turnCameraWithHorseInFirstPerson.toString(),
            "verbose_damage" to b(state.verboseDamage),
            "verbose_shot_difficulty" to b(state.verboseShotDifficulty),
            "battle_size" to f(state.battleSize),
            "attack_direction_control" to state.attackDirectionControl.toString(),
            "defend_direction_control" to state.defendDirectionControl.toString(),
            "lance_control" to state.lanceControl.toString(),
            "anisotropic_filtering" to b(state.anisotropicFiltering),
            "enable_environment_shadows" to b(state.enableEnvironmentShadows),
            "verbose_casualties" to b(state.verboseCasualties),
            "verbose_experience" to b(state.verboseExperience),
            "realistic_shadows_on_plants" to state.realisticShadowsOnPlants.toString(),
            "number_of_ragdolls" to state.numberOfRagdolls.toString(),
            "gamma" to f(state.gamma),
            "character_detail" to f(state.characterDetail),
            "character_shadow_detail" to f(state.characterShadowDetail),
            "control_mouse_movement_y_scale" to f(state.controlMouseMovementYScale),
            "blood_stains" to state.bloodStains.toString(),
            "use_winmm_audio" to b(state.useWinmmAudio),
            "enable_version_check" to b(state.enableVersionCheck),
            "enable_aspect_ratio_control" to b(state.enableAspectRatioControl),
            "realistic_headshots" to b(state.realisticHeadshots),
            "auto_gfx_quality" to b(state.autoGfxQuality),
            "enable_gamepad_vibration" to b(state.enableGamepadVibration)
        )

        val sb = StringBuilder(2200)
        for ((k, v) in entries) {
            sb.append(k).append(" = ").append(v).append("\n\n")
        }
        for ((k, v) in state.extraEntries) {
            sb.append(k).append(" = ").append(v).append("\n\n")
        }
        return sb.toString()
    }

    val KNOWN_KEYS: Set<String> = setOf(
        "first_time",
        "texture_detail",
        "render_buffer_size",
        "max_framerate",
        "start_windowed",
        "use_pixel_shaders",
        "use_vertex_shaders",
        "fake_reflections",
        "show_framerate",
        "use_ondemand_textures_",
        "use_ondemand_textures_mt",
        "disable_music",
        "disable_sound",
        "disable_frequency_variation",
        "cheat_mode",
        "enable_blood",
        "enable_edit_mode",
        "force_single_threading",
        "debug_mode",
        "display_width",
        "display_height",
        "display_bits",
        "antialiasing",
        "sample_quality",
        "alpha2coverage",
        "force_vsync",
        "shadowmap_quality",
        "shader_quality",
        "postfx_dof",
        "postfx_hdr",
        "postfx_autoexp",
        "flora_degrade_distance",
        "flora_lod_detail",
        "use_instancing",
        "use_secure_connection",
        "max_number_of_connections",
        "look_for_server_on_this_machine",
        "music_volume",
        "sound_volume",
        "mouse_sensitivity",
        "invert_mouse",
        "enable_lighting",
        "enable_particles",
        "enable_character_shadows",
        "enable_accurate_shadows",
        "number_of_corpses",
        "grass_density",
        "combat_speed",
        "friend_combat_difficulty",
        "adapter_format",
        "reduce_combat_ai",
        "reduce_campaign_ai",
        "combat_difficulty",
        "display_labels",
        "display_targeting_reticule",
        "display_attack_direction",
        "turn_camera_with_horse_in_first_person",
        "verbose_damage",
        "verbose_shot_difficulty",
        "battle_size",
        "attack_direction_control",
        "defend_direction_control",
        "lance_control",
        "anisotropic_filtering",
        "enable_environment_shadows",
        "verbose_casualties",
        "verbose_experience",
        "realistic_shadows_on_plants",
        "number_of_ragdolls",
        "gamma",
        "character_detail",
        "character_shadow_detail",
        "control_mouse_movement_y_scale",
        "blood_stains",
        "use_winmm_audio",
        "enable_version_check",
        "enable_aspect_ratio_control",
        "realistic_headshots",
        "auto_gfx_quality",
        "enable_gamepad_vibration"
    )
}

/**
 * Reads and writes `rgl_config.txt` in the recompiled game's user profile Documents folder
 * (`filesDir/recomp/r4/user/Documents/Mount&Blade Warband/rgl_config.txt`).
 */
internal object RglConfigStore {
    private const val RELATIVE_PATH = "recomp/r4/user/Documents/Mount&Blade Warband/rgl_config.txt"

    fun configFile(filesDir: File): File = File(filesDir, RELATIVE_PATH)

    fun loadOrInitialize(filesDir: File): Pair<RglConfigState, Boolean> {
        val file = configFile(filesDir)
        if (file.isFile) {
            return try {
                RglConfigCodec.parse(file.readText(Charsets.UTF_8)) to true
            } catch (_: Throwable) {
                RglConfigState() to false
            }
        }
        val defaults = RglConfigState()
        val saved = save(filesDir, defaults)
        return defaults to saved
    }

    fun save(filesDir: File, state: RglConfigState): Boolean {
        return try {
            val file = configFile(filesDir)
            file.parentFile?.mkdirs()
            val tmp = File(file.parentFile, "${file.name}.tmp")
            tmp.writeText(RglConfigCodec.serialize(state), Charsets.UTF_8)
            if (file.exists() && !tmp.renameTo(file)) {
                tmp.copyTo(file, overwrite = true)
                tmp.delete()
            } else if (!file.exists()) {
                if (!tmp.renameTo(file)) {
                    tmp.copyTo(file, overwrite = true)
                    tmp.delete()
                }
            }
            true
        } catch (_: Throwable) {
            false
        }
    }
}
