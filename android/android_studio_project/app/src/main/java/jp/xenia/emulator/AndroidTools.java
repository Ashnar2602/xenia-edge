package jp.xenia.emulator;

/** Thin calls to desktop services; invoked on the owning activity's serial worker. */
final class AndroidTools {
    static native String[][] listNative(long context, String page, int title, long xuid);
    static native void changeNative(
            long context, String operation, int title, long xuid, String key, String value);
    static native void iconNative(long context, long xuid, byte[] png);
    static boolean packageNative(long context, String path, boolean extract) {
        return installPackageNative(context, path, extract, 0);
    }
    static native boolean installPackageNative(long context, String path, boolean extract, long session);
    static native long beginInstallNative();
    static native long[] installProgressNative(long session, boolean cancel);
    static native void endInstallNative(long session);
    private AndroidTools() {}
}
