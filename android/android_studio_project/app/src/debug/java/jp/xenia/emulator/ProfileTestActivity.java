package jp.xenia.emulator;

import android.os.Bundle;
import android.widget.Button;
import java.io.File;
import java.util.UUID;
import java.util.concurrent.Executors;

/** Runs the real profile flow in a fresh process with disposable storage. */
public class ProfileTestActivity extends WindowedAppActivity {
    private static native String settingsNative();
    static final java.util.List<String> sessionEvents = new java.util.ArrayList<>();
    @Override
    protected void sessionEvent(int event, String[] values) {
        sessionEvents.add(event + ":" + java.util.Arrays.toString(values));
    }
    @Override
    protected String getWindowedAppIdentifier() {
        return "xenia";
    }

    @Override
    protected void onCreate(Bundle saved) {
        String id = UUID.fromString(getIntent().getStringExtra("test_id")).toString();
        File storage = new File(getCacheDir(), "profile-test-" + id);
        storage.mkdirs();
        Bundle cvars = new Bundle();
        cvars.putString("storage_root", storage.getAbsolutePath());
        if (getIntent().getBooleanExtra("test_features", false)) {
            cvars.putString("log_file", "feature.log");
            cvars.putBoolean("log_append", getIntent().getBooleanExtra("verify_features", false));
        }
        String game = getIntent().getStringExtra("test_game_path");
        if (game != null) {
            cvars.putString("android_game_path", game);
            cvars.putInt("android_resolution", 1);
            cvars.putInt("android_fps_limit", 0);
        }
        getIntent().putExtra(EXTRA_CVARS, cvars);
        super.onCreate(saved);
        if (getNativeAppContext() == 0)
            return;
        Button state = new Button(this);
        state.setText("PROFILE_PENDING");
        state.setOnClickListener(v -> finish());
        setContentView(state);
        if (getIntent().getBooleanExtra("test_features", false)) {
            Executors.newSingleThreadExecutor().execute(() -> {
                String result;
                try {
                    FeatureChecks.run(getNativeAppContext(), storage,
                            getIntent().getBooleanExtra("verify_features", false));
                    result = "FEATURES_OK";
                } catch (Throwable e) {
                    result = "FEATURES_ERROR: " + e;
                    android.util.Log.e("xenia", "Feature test", e);
                }
                try {
                    java.nio.file.Path temporary =
                            new File(storage, "features.result.tmp").toPath();
                    java.nio.file.Files.write(
                            temporary, result.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    java.nio.file.Files.move(temporary,
                            new File(storage, "features.result").toPath(),
                            java.nio.file.StandardCopyOption.ATOMIC_MOVE);
                } catch (java.io.IOException e) {
                    android.util.Log.e("xenia", "Feature result", e);
                }
                final String output = result;
                runOnUiThread(() -> { state.setText(output); finish(); });
            });
            return;
        }
        if (game != null) {
            state.setText(settingsNative());
            return;
        }
        new AndroidProfiles(this, getNativeAppContext(), Executors.newSingleThreadExecutor(),
                () -> state.setText("PROFILE_READY"), () -> state.setText("PROFILE_CANCELLED"))
                .ensureSignedIn();
    }

    @Override
    @android.annotation.SuppressLint("MissingSuperCall")
    protected void onDestroy() {
        // Same process isolation as the player, including process-restart tests.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
