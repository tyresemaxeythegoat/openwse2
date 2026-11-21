package org.wse2.android.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RglConfigTest {

    @Test
    fun roundTripsAllKnownKeysAndPreservesExtras() {
        val sample = """
            first_time = 0

            texture_detail = 85

            render_buffer_size = 64

            max_framerate = 120

            use_ondemand_textures_ = 1

            use_ondemand_textures_mt = 0

            enable_blood = 0

            enable_blood = 1

            battle_size = 0.5000

            music_volume = 0.4000

            gamma = 2.2500

            custom_user_flag = 42
        """.trimIndent()

        val parsed = RglConfigCodec.parse(sample)
        assertEquals(85, parsed.textureDetail)
        assertEquals(64, parsed.renderBufferSize)
        assertEquals(120, parsed.maxFramerate)
        assertTrue(parsed.useOnDemandTextures)
        assertFalse(parsed.useOnDemandTexturesMt)
        assertTrue(parsed.enableBlood)
        assertEquals(90, parsed.battleSizeTroops)
        assertEquals(0.4f, parsed.musicVolume, 0.0001f)
        assertEquals(2.25f, parsed.gamma, 0.0001f)
        assertEquals("42", parsed.extraEntries["custom_user_flag"])

        val serialized = RglConfigCodec.serialize(parsed)
        assertTrue(serialized.contains("use_ondemand_textures_ = 1"))
        assertTrue(serialized.contains("gamma = 2.2500"))
        assertTrue(serialized.contains("custom_user_flag = 42"))
        // Duplicate enable_blood should be deduplicated on serialize.
        val bloodOccurrences = Regex("""(?m)^enable_blood\s*=""").findAll(serialized).count()
        assertEquals(1, bloodOccurrences)

        val reparsed = RglConfigCodec.parse(serialized)
        assertEquals(parsed, reparsed)
    }

    @Test
    fun knownKeyCountCoversFullRetailConfig() {
        assertEquals(80, RglConfigCodec.KNOWN_KEYS.size)
        val serialized = RglConfigCodec.serialize(RglConfigState())
        for (key in RglConfigCodec.KNOWN_KEYS) {
            assertTrue("Missing serialized key: $key", serialized.contains("$key = "))
        }
    }

    @Test
    fun qualityPresetsAdjustGraphicsFields() {
        val base = RglConfigState()
        val low = base.withQualityPreset("low")
        val high = base.withQualityPreset("high")
        assertTrue(low.textureDetail < high.textureDetail)
        assertTrue(low.shaderQuality < high.shaderQuality)
        assertFalse(low.enableCharacterShadows)
        assertTrue(high.enableCharacterShadows)
        assertEquals(150, high.battleSizeTroops)
    }
}
