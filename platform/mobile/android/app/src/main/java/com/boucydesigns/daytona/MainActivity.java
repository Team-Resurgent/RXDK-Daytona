package com.boucydesigns.daytona;

import org.libsdl.app.SDLActivity;

public final class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // SDL's Java bootstrap first, then the CMake target whose OUTPUT_NAME is "main".
        return new String[] { "SDL3", "main" };
    }
}
