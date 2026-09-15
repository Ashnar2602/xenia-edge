package jp.xenia.emulator;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.file.Files;

/** Real desktop services with disposable storage, never the user's account. */
final class FeatureChecks {
    static native byte[] historyFixtureNative(int title, long seconds);
    private static native String keyboardNative(long context);
    private static native void logNative();
    private static native boolean servicesNative(long context);
    private static void check(boolean value, String message) {
        if (!value)
            throw new AssertionError(message);
    }
    private static String[] field(String[][] rows, String key) {
        for (String[] row : rows)
            if (row[0].equals(key))
                return row;
        throw new AssertionError("Missing " + key);
    }
    interface Action {
        void run() throws Exception;
    }
    private static void rejects(Action action) throws Exception {
        boolean failed = false;
        try {
            action.run();
        } catch (Exception e) {
            failed = true;
        }
        check(failed, "Invalid operation accepted");
    }
    static void run(long context, File root, boolean verify) throws Exception {
        logNative();
        File log = new File(root, "feature.log");
        String logs = new String(
                Files.readAllBytes(log.toPath()), java.nio.charset.StandardCharsets.UTF_8);
        check(logs.contains("Android logging fixture"), "File logging enabled");
        check(logs.split("Android logging fixture", -1).length == (verify ? 3 : 2),
                "Log append on process restart");
        if (!verify) {
            rejects(() -> AndroidTools.changeNative(context, "create", 0, 0, "", "1Invalid"));
            for (int i = 0; i < 4; i++)
                AndroidTools.changeNative(
                        context, "create", 0, 0, "", "Feature " + (char) ('A' + i));
            String[][] profiles = AndroidTools.listNative(context, "profiles", 0, 0);
            check(profiles.length == 4, "Four accounts");
            for (int i = 0; i < 4; i++)
                AndroidTools.changeNative(context, "login", 0,
                        Long.parseUnsignedLong(profiles[i][0], 16), "", Integer.toString(i));
        }
        String[][] profiles = AndroidTools.listNative(context, "profiles", 0, 0);
        check(profiles.length == 4, "Accounts persisted");
        java.util.Set<String> slots = new java.util.HashSet<>();
        for (String[] row : profiles) slots.add(row[2]);
        check(slots.equals(new java.util.HashSet<>(java.util.Arrays.asList("1", "2", "3", "4"))),
                "Distinct sign-in slots: " + slots);
        long xuid = Long.parseUnsignedLong(profiles[0][0], 16);
        if (!verify) {
            AndroidTools.changeNative(context, "profile", 0, xuid, "motto", "Hello Android");
            AndroidTools.changeNative(context, "profile", 0, xuid, "zone", "2");
            rejects(() -> AndroidTools.changeNative(context, "profile", 0, xuid, "country", "999"));
            rejects(() -> AndroidTools.changeNative(context, "login", 0, xuid, "", "4"));
        }
        String[][] profile = AndroidTools.listNative(context, "profile", 0, xuid);
        check(field(profile, "motto")[1].equals("Hello Android"),
                "GPD text persisted: " + field(profile, "motto")[1]);
        check(field(profile, "zone")[1].equals("2"), "GPD zone persisted");
        check(field(profile, "online_xuid")[2].equals("readonly"), "Online XUID is read-only");
        check(field(profile, "online_xuid")[1].matches("[0-9a-fA-F]{16}"), "Online XUID format");
        check(field(profile, "online_domain")[2].equals("readonly"), "Online domain is read-only");
        if (!verify) {
            rejects(()
                            -> AndroidTools.changeNative(
                                    context, "profile", 0, xuid, "online_xuid", "1"));
            rejects(()
                            -> AndroidTools.changeNative(
                                    context, "profile", 0, xuid, "online_domain", "invalid"));
            String[][] unchanged = AndroidTools.listNative(context, "profile", 0, xuid);
            check(field(profile, "online_xuid")[1].equals(field(unchanged, "online_xuid")[1]),
                    "Online XUID unchanged");
            check(field(profile, "online_domain")[1].equals(field(unchanged, "online_domain")[1]),
                    "Online domain unchanged");
        }
        if (verify)
            return;
        check(servicesNative(context), "Shared service callbacks and save header");
        check(ProfileTestActivity.sessionEvents.equals(
                      java.util.Arrays.asList("0:[/fixture/game.zar, next.xex, 123, 00FF]", "1:[]",
                              "2:[Fixture title, 1096026071, ]", "3:[2]", "5:[1]", "5:[0]")),
                "JNI session callbacks: " + ProfileTestActivity.sessionEvents);
        String[][] saves = AndroidTools.listNative(context, "saves", 0x415407D7, 0);
        check(saves.length == 1 && saves[0][1].equals("Friendly save"),
                "Extracted save sidecar name");
        Files.write(new File(saves[0][3], "data").toPath(), new byte[] {1, 2, 3, 4, 5});
        saves = AndroidTools.listNative(context, "saves", 0x415407D7, 0);
        check(saves[0][5].equals("5"), "Extracted save size");
        String keyboard = keyboardNative(context);
        check(keyboard.equals("OK"), keyboard);
        android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(
                64, 64, android.graphics.Bitmap.Config.ARGB_8888);
        java.io.ByteArrayOutputStream png = new java.io.ByteArrayOutputStream();
        bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, png);
        bitmap.recycle();
        AndroidTools.iconNative(context, xuid, png.toByteArray());
        rejects(() -> AndroidTools.iconNative(context, xuid, new byte[] {1, 2, 3}));
        AndroidTools.iconNative(context, xuid, null);
        int title = 0x415407D7;
        String[][] patches = AndroidTools.listNative(context, "patches", title, 0);
        check(patches.length > 0, "Bundled Catherine patches");
        String[] patch = patches[0];
        AndroidTools.changeNative(context, "patch", title, 0, patch[0],
                patch[3] + ":" + !Boolean.parseBoolean(patch[4]));
        File override = new File(root, "patches/" + patch[0]);
        byte[] before = Files.readAllBytes(override.toPath());
        rejects(()
                        -> AndroidTools.changeNative(
                                context, "patch", title, 0, patch[0], "999999:true"));
        check(java.util.Arrays.equals(before, Files.readAllBytes(override.toPath())),
                "Failed patch edit preserved file");
        AndroidTools.changeNative(context, "patch", title, 0, patch[0], patch[3] + ":" + patch[4]);
        for (int type : new int[] {0x00000002, 0x000B0000}) {
            ByteBuffer header = ByteBuffer.allocate(0xA000);
            header.putInt(0, 0x4C495645)
                    .putInt(0x344, type)
                    .putInt(0x348, 1)
                    .putInt(0x358, 2)
                    .putInt(0x360, title);
            File original = new File(root, "package-" + type);
            Files.write(original.toPath(), header.array());
            if (type == 0x000B0000)
                rejects(()
                                -> AndroidTools.changeNative(context, "content_add", title, 0, "1",
                                        original.getAbsolutePath()));
            rejects(()
                            -> AndroidTools.changeNative(context, "content_add", 0x12345678, 0, "0",
                                    original.getAbsolutePath()));
            AndroidTools.changeNative(
                    context, "content_add", title, 0, "0", original.getAbsolutePath());
            AndroidTools.changeNative(
                    context, "content_add", title, 0, "0", original.getAbsolutePath());
            String[][] content = AndroidTools.listNative(context, "content", title, 0);
            check(content.length == 1, "Duplicate content avoided");
            check(Long.parseLong(content[0][5]) == header.capacity(), "Package size");
            File link = new File(content[0][3]);
            check(Files.isSymbolicLink(link.toPath()), "Zero-copy link");
            check(link.getCanonicalFile().equals(original.getCanonicalFile()), "Original target");
            AndroidTools.changeNative(
                    context, "content_remove", title, 0, link.getAbsolutePath(), "");
            check(original.isFile() && original.length() == header.capacity(),
                    "Detach preserves original");
            check(AndroidTools.listNative(context, "content", title, 0).length == 0,
                    "Detached listing");
            check(AndroidTools.packageNative(context, original.getAbsolutePath(), false),
                    "General zero-copy import");
            check(AndroidTools.packageNative(context, original.getAbsolutePath(), false),
                    "General duplicate is idempotent");
            check(Files.isSymbolicLink(link.toPath())
                            && link.getCanonicalFile().equals(original.getCanonicalFile()),
                    "General import original preserved");
            AndroidTools.changeNative(
                    context, "content_remove", title, 0, link.getAbsolutePath(), "");
            Files.write(link.toPath(), new byte[] {42});
            rejects(() -> AndroidTools.packageNative(context, original.getAbsolutePath(), false));
            check(Files.readAllBytes(link.toPath())[0] == 42, "Existing data not overwritten");
            Files.delete(link.toPath());
            long session = AndroidTools.beginInstallNative();
            AndroidTools.installProgressNative(session, true);
            rejects(() -> AndroidTools.installPackageNative(
                    context, original.getAbsolutePath(), false, session));
            check(!Files.exists(link.toPath()), "Cancelled import leaves no destination");
            AndroidTools.endInstallNative(session);
            long next = AndroidTools.beginInstallNative();
            try {
                AndroidTools.installProgressNative(session, true);
                check(AndroidTools.installProgressNative(next, false)[2] == 0,
                        "Late cancellation cannot affect next batch");
                check(AndroidTools.installPackageNative(context,
                                original.getAbsolutePath(), false, next), "Next batch succeeds");
            } finally {
                AndroidTools.endInstallNative(next);
            }
            AndroidTools.changeNative(context, "content_remove", title, 0, link.getAbsolutePath(), "");
        }
        ByteBuffer mutableHeader = ByteBuffer.allocate(0xA000);
        mutableHeader.putInt(0, 0x4C495645).putInt(0x344, 1).putInt(0x348, 1).putInt(0x360, title);
        File mutable = new File(root, "mutable-save");
        Files.write(mutable.toPath(), mutableHeader.array());
        check(!AndroidTools.packageNative(context, mutable.getAbsolutePath(), false),
                "Mutable import requires explicit extraction");
        check(mutable.length() == mutableHeader.capacity(), "Mutable original preserved");
        check(AndroidTools.listNative(context, "stats", title, 0).length >= 1,
                "Compatibility service");
    }
}
