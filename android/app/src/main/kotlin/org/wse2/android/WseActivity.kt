package org.wse2.android

import org.libsdl.app.SDLActivity

class WseActivity : SDLActivity() {
    override fun getLibraries(): Array<String> = arrayOf("SDL3", "wse2")
}
