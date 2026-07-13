package com.whl.quickjs.wrapper;

import java.io.*;
import java.nio.file.*;

final class QuickJSNativeLoader {

    private static final String LIB_NAME = "quickjs-java-wrapper";
    private static volatile boolean loaded;

    private QuickJSNativeLoader() {}

    static boolean isLoaded() { return loaded; }

    static synchronized void load() {
        if (loaded) return;

        // 1. Try java.library.path first
        try {
            System.loadLibrary(LIB_NAME);
            loaded = true;
            return;
        } catch (UnsatisfiedLinkError ignored) {}

        // 2. Try bundled native lib from classpath
        String platform = detectPlatform();
        if (platform != null) {
            String dir = "native/" + platform + "/";
            String libName = mapLibName(platform);
            try (InputStream is = QuickJSNativeLoader.class.getClassLoader().getResourceAsStream(dir + libName)) {
                if (is != null) {
                    Path tempDir = Files.createTempDirectory("quickjs-java-wrapper-");
                    tempDir.toFile().deleteOnExit();

                    Path libFile = tempDir.resolve(libName);
                    Files.copy(is, libFile, StandardCopyOption.REPLACE_EXISTING);
                    libFile.toFile().deleteOnExit();

                    // Copy platform dependencies (e.g. libwinpthread-1.dll for MinGW-w64)
                    copyDependencies(platform, dir, tempDir);

                    System.load(libFile.toAbsolutePath().toString());
                    loaded = true;
                    return;
                }
            } catch (IOException | UnsatisfiedLinkError ignored) {}
        }

        throw new QuickJSException(
            "Failed to load native library for platform '" + detectPlatform() + "'. " +
            "Expected classpath resource: native/" + detectPlatform() + "/" + mapLibName(detectPlatform()) + ". " +
            "Make sure the native library is on java.library.path or bundled in the JAR."
        );
    }

    private static void copyDependencies(String platform, String dir, Path tempDir) {
        if (!platform.startsWith("windows")) return;
        String[] deps = {"libwinpthread-1.dll"};
        for (String dep : deps) {
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
