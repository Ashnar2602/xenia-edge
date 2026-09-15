package jp.xenia.emulator;

import android.content.Intent;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ProgressBar;

/** The existing Vulkan trace viewer, with Android surface/lifecycle handling. */
public final class TraceActivity extends WindowedAppActivity {
    private boolean started;
    private native boolean prepareNative(long context);
    @Override
    protected String getWindowedAppIdentifier() {
        return "xenia_gpu_vulkan_trace_viewer";
    }
    @Override
    protected void onCreate(Bundle saved) {
        Bundle cvars = new Bundle();
        cvars.putString("storage_root", getFilesDir().getAbsolutePath());
        cvars.putString("target_trace_file", getIntent().getStringExtra("trace_path"));
        getIntent().putExtra(EXTRA_CVARS, cvars);
        super.onCreate(saved);
        if (getNativeAppContext() == 0)
            return;
        FrameLayout frame = new FrameLayout(this);
        WindowSurfaceView surface = new WindowSurfaceView(this, null);
        frame.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        ProgressBar progress = new ProgressBar(this);
        frame.addView(progress, new FrameLayout.LayoutParams(-2, -2, android.view.Gravity.CENTER));
        setContentView(frame);
        setWindowSurfaceView(surface);
        getWindow().getInsetsController().hide(android.view.WindowInsets.Type.systemBars());
        getWindow().getInsetsController().setSystemBarsBehavior(
                android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {}
            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {}
            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                if (started || width <= 0 || height <= 0)
                    return;
                started = true;
                new Thread(() -> {
                    boolean ready = prepareNative(getNativeAppContext());
                    runOnUiThread(() -> {
                        progress.setVisibility(View.GONE);
                        if (ready) surface.setContentDescription(getString(R.string.open_trace));
                        else
                            new android.app.AlertDialog.Builder(TraceActivity.this)
                                    .setMessage(R.string.trace_failed)
                                    .setPositiveButton(android.R.string.ok, (d, w) -> finish())
                                    .show();
                    });
                }, "Trace loader").start();
            }
        });
    }
    @Override
    public void onBackPressed() {
        startActivity(new Intent(this, LauncherActivity.class));
        finish();
    }
    @Override
    @android.annotation.SuppressLint("MissingSuperCall")
    protected void onDestroy() {
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
