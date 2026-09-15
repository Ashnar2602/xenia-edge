package jp.xenia.emulator;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.widget.ProgressBar;

/** Main-process handoff: never reuse a dying :emulation process. */
public final class SessionActivity extends LocalizedActivity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private long deadline;
    private final Runnable resume = new Runnable() {
        @Override
        public void run() {
            if (isFinishing())
                return;
            int pid = getIntent().getIntExtra("previous_pid", 0);
            if (pid > 0 && new java.io.File("/proc/" + pid).exists()) {
                if (SystemClock.uptimeMillis() < deadline) {
                    handler.postDelayed(this, 100);
                    return;
                }
                new android.app.AlertDialog.Builder(SessionActivity.this)
                        .setMessage(R.string.data_busy)
                        .setPositiveButton(android.R.string.ok, (d, w) -> finish())
                        .show();
                return;
            }
            String path = getIntent().getStringExtra("game_path");
            Intent next = new Intent(SessionActivity.this,
                    path == null ? LauncherActivity.class : EmulatorActivity.class);
            if (path != null) {
                next.putExtras(getIntent()).putExtra("game_name", new java.io.File(path).getName());
                android.content.SharedPreferences settings =
                        getSharedPreferences("android_settings", MODE_PRIVATE);
                next.putExtra("resolution", settings.getInt("resolution", -1))
                        .putExtra("fps_limit", settings.getInt("fps_limit", -1))
                        .putExtra("touch_controls", settings.getBoolean("touch_controls", true));
            }
            startActivity(next);
            finish();
        }
    };
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        deadline = SystemClock.uptimeMillis() + 15000;
        setContentView(new ProgressBar(this));
        handler.post(resume);
    }
    @Override
    protected void onDestroy() {
        handler.removeCallbacks(resume);
        super.onDestroy();
    }
}
