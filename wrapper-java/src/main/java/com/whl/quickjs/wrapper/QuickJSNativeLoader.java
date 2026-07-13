package com.whl.quickjs.wrapper;

import java.io.*;
import java.nio.file.*;

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

                    copyDependencies(platform, dir, tempDir);

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

    private static void copyDependencies(String platform, String dir, Path tempDir) {
        if (!platform.startsWith("windows")) return;
        // Copy all .dll files from the native directory to support any runtime dependencies
        String[] known = {"libwinpthread-1.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll"};
        for (String dep : known) {
            try (InputStream dis = QuickJSNativeLoader.class.getClassLoader().getResourceAsStream(dir + dep)) {
                if (dis != null) {
                    Path depFile = tempDir.resolve(dep);
                    Files.copy(dis, depFile, StandardCopyOption.REPLACE_EXISTING);
                    depFile.toFile().deleteOnExit();
                }
            } catch (Exception ignored) {}
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
