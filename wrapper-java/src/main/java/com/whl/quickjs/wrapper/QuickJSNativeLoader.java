package com.whl.quickjs.wrapper;

import java.io.*;
import java.nio.file.*;

final class QuickJSNativeLoader {

    private static final String LIB_NAME = "quickjs-java-wrapper";
    private static volatile boolean loaded;

    private QuickJSNativeLoader() {}

    static boolean isLoaded() {
        return loaded;
    }

    static synchronized void load() {
        if (loaded) return;

        // 1. Try java.library.path first
        try {
            System.loadLibrary(LIB_NAME);
            loaded = true;
            return;
        } catch (UnsatisfiedLinkError ignored) {
        }

        // 2. Try bundled native lib from classpath
        String platform = detectPlatform();
        if (platform != null) {
            String libPath = "native/" + platform + "/" + mapLibName(platform);
            try (InputStream is = QuickJSNativeLoader.class.getClassLoader().getResourceAsStream(libPath)) {
                if (is != null) {
                    Path tempDir = Files.createTempDirectory("quickjs-java-wrapper-");
                    tempDir.toFile().deleteOnExit();

                    if (platform.startsWith("windows")) {
                        String depPath = "native/" + platform + "/libwinpthread-1.dll";
                        extractResource(tempDir, depPath);
                    }

                    Path tempFile = tempDir.resolve(mapLibName(platform));
                    Files.copy(is, tempFile, StandardCopyOption.REPLACE_EXISTING);
                    System.load(tempFile.toAbsolutePath().toString());
                    loaded = true;
                    return;
                }
            } catch (IOException | UnsatisfiedLinkError ignored) {
            }
        }

        throw new QuickJSException(
            "Failed to load native library '" + mapLibName(detectPlatform()) + "'. " +
            "Make sure the native library is on java.library.path or bundled in the JAR."
        );
    }

    private static void extractResource(Path targetDir, String resourcePath) {
        try (InputStream is = QuickJSNativeLoader.class.getClassLoader().getResourceAsStream(resourcePath)) {
            if (is != null) {
                String name = resourcePath.substring(resourcePath.lastIndexOf('/') + 1);
                Files.copy(is, targetDir.resolve(name), StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException ignored) {
        }
    }

    private static String detectPlatform() {
        String os = System.getProperty("os.name", "").toLowerCase();
        String arch = System.getProperty("os.arch", "").toLowerCase();

        if (arch.contains("amd64") || arch.contains("x86_64") || arch.contains("x64")) {
            arch = "x86_64";
        } else if (arch.contains("aarch64") || arch.contains("arm64")) {
            arch = "arm64";
        } else {
            return null;
        }

        if (os.contains("win")) {
            return "windows-" + arch;
        } else if (os.contains("mac") || os.contains("darwin")) {
            return "macos-" + arch;
        } else if (os.contains("nux") || os.contains("nix")) {
            return "linux-" + arch;
        }
        return null;
    }

    private static String mapLibName(String platform) {
        if (platform == null) return LIB_NAME;
        if (platform.startsWith("windows")) return "lib" + LIB_NAME + ".dll";
        if (platform.startsWith("macos")) return "lib" + LIB_NAME + ".dylib";
        return "lib" + LIB_NAME + ".so";
    }

    private static String libExt(String platform) {
        if (platform == null) return "";
        if (platform.startsWith("windows")) return ".dll";
        if (platform.startsWith("macos")) return ".dylib";
        return ".so";
    }
}
