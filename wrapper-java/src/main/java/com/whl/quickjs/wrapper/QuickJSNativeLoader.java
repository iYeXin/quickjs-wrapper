package com.whl.quickjs.wrapper;

import java.io.*;
import java.nio.file.*;
import java.util.*;

final class QuickJSNativeLoader {

    private static final String LIB_NAME = "quickjs-java-wrapper";
    private static volatile boolean loaded;
    private static volatile String loadError;

    private QuickJSNativeLoader() {}

    static boolean isLoaded() { return loaded; }
    static String getLoadError() { return loadError; }

    static synchronized void load() {
        if (loaded) return;

        // 1. Try java.library.path first
        try {
            System.loadLibrary(LIB_NAME);
            loaded = true;
            return;
        } catch (UnsatisfiedLinkError e) {
            loadError = "java.library.path: " + e.getMessage();
        }

        // 2. Try bundled native lib from classpath
        String platform = detectPlatform();
        if (platform != null) {
            String dir = "native/" + platform + "/";
            String libName = mapLibName(platform);
            String resourcePath = dir + libName;
            try (InputStream is = QuickJSNativeLoader.class.getClassLoader().getResourceAsStream(resourcePath)) {
                if (is != null) {
                    Path tempDir = Files.createTempDirectory("quickjs-java-wrapper-");
                    tempDir.toFile().deleteOnExit();

                    Path libFile = tempDir.resolve(libName);
                    Files.copy(is, libFile, StandardCopyOption.REPLACE_EXISTING);
                    libFile.toFile().deleteOnExit();

                    extractAndLoadDeps(platform, dir, tempDir, libName);

                    try {
                        System.load(libFile.toAbsolutePath().toString());
                        loaded = true;
                        return;
                    } catch (UnsatisfiedLinkError e) {
                        loadError = "System.load(" + libFile + "): " + e.getMessage();
                    }
                } else {
                    loadError = "Resource not found on classpath: " + resourcePath;
                }
            } catch (IOException e) {
                loadError = "Extraction failed: " + e.getMessage();
            }
        } else {
            loadError = "Unsupported platform: " + System.getProperty("os.name") + " " + System.getProperty("os.arch");
        }

        throw new QuickJSException(
            "Failed to load native library for platform '" + detectPlatform() + "'. " +
            "Expected: native/" + detectPlatform() + "/" + mapLibName(detectPlatform()) + ". " +
            (loadError != null ? "Cause: " + loadError + ". " : "") +
            "Make sure the native library is on java.library.path or bundled in the JAR."
        );
    }

    /**
     * Extract known companion libs to temp dir.
     * On Windows, the DLL loader searches the loaded DLL's directory for
     * dependencies, so copying them alongside is sufficient.
     * On Linux/macOS, the dynamic linker does NOT search the loaded lib's
     * directory by default, so we must System.load() each dependency first
     * to register it in the process address space before loading the main lib.
     */
    private static void extractAndLoadDeps(String platform, String dir, Path tempDir, String mainLibName) {
        String[][] knownDeps;
        if (platform.startsWith("windows")) {
            knownDeps = new String[][]{
                {"libwinpthread-1.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll"},
                {"libgcc_s_dw2-1.dll", "libwinpthread-2.dll"}  // 32-bit fallback names
            };
        } else if (platform.startsWith("linux")) {
            knownDeps = new String[][]{
                {"libquickjs.so", "libquickjs.so.0"},
            };
        } else if (platform.startsWith("macos")) {
            knownDeps = new String[][]{
                {"libquickjs.dylib"},
            };
        } else {
            knownDeps = new String[0][];
        }

        boolean isWindows = platform.startsWith("windows");

        for (String[] group : knownDeps) {
            for (String dep : group) {
                try (InputStream dis = QuickJSNativeLoader.class.getClassLoader().getResourceAsStream(dir + dep)) {
                    if (dis == null) continue;

                    Path depFile = tempDir.resolve(dep);
                    Files.copy(dis, depFile, StandardCopyOption.REPLACE_EXISTING);
                    depFile.toFile().deleteOnExit();

                    // On non-Windows: must load dependency into process first
                    // so the dynamic linker finds it when loading the main lib.
                    // On Windows: DLL search path includes the loaded DLL's
                    // directory, so just copying alongside is sufficient.
                    if (!isWindows) {
                        System.load(depFile.toAbsolutePath().toString());
                    }
                } catch (Exception ignored) {}
            }
        }
    }

    private static String detectPlatform() {
        String os = System.getProperty("os.name", "").toLowerCase();
        String arch = System.getProperty("os.arch", "").toLowerCase();
        if (arch.contains("amd64") || arch.contains("x86_64") || arch.contains("x64")) arch = "x86_64";
        else if (arch.contains("aarch64") || arch.contains("arm64")) arch = "arm64";
        else return null;
        if (os.contains("win")) return "windows-" + arch;
        if (os.contains("mac") || os.contains("darwin")) return "macos-" + arch;
        if (os.contains("nux") || os.contains("nix")) return "linux-" + arch;
        return null;
    }

    private static String mapLibName(String platform) {
        if (platform == null) return LIB_NAME;
        if (platform.startsWith("windows")) return "lib" + LIB_NAME + ".dll";
        if (platform.startsWith("macos")) return "lib" + LIB_NAME + ".dylib";
        return "lib" + LIB_NAME + ".so";
    }
}
