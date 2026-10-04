package com.sampreeth.nfscarbon;

import org.libsdl.app.SDLActivity;

/**
 * Loads the recompiled game library. SDL3 is linked statically into
 * libnfscarbon.so, so the default SDL3/main library list is replaced.
 */
public class NfsCarbonActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "nfscarbon" };
    }
}
