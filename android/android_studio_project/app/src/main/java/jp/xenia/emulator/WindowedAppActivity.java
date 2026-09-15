package jp.xenia.emulator;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import jp.xenia.XeniaRuntimeException;
import org.jetbrains.annotations.Nullable;

public abstract class WindowedAppActivity extends LocalizedActivity {
    // The EXTRA_CVARS value literal is also used in the native code.

    /**
     * Name of the Bundle intent extra containing Xenia config variable launch arguments.
     */
    public static final String EXTRA_CVARS = "jp.xenia.emulator.WindowedAppActivity.EXTRA_CVARS";

    static {
        System.loadLibrary("xenia-app");
    }

    private final WindowSurfaceListener mWindowSurfaceListener = new WindowSurfaceListener();

    // May be 0 while destroying (mainly while the superclass is).
    private long mAppContext = 0;
    private DataLock dataLock;

    protected final long getNativeAppContext() {
        return mAppContext;
    }

    private static final int NATIVE_FILES = 7301;
    private boolean nativeDirectory;
    private native void filesSelectedNative(long context, String[] paths);

    protected void pickNativeFiles(int mode, int type, boolean multiple) {
        nativeDirectory = type == 1;
        android.content.Intent intent = new android.content.Intent(nativeDirectory
                        ? android.content.Intent.ACTION_OPEN_DOCUMENT_TREE
                        : mode == 1 ? android.content.Intent.ACTION_CREATE_DOCUMENT
                                    : android.content.Intent.ACTION_OPEN_DOCUMENT);
        if (!nativeDirectory)
            intent.setType("*/*")
                    .addCategory(android.content.Intent.CATEGORY_OPENABLE)
                    .putExtra(android.content.Intent.EXTRA_ALLOW_MULTIPLE, multiple);
        try {
            startActivityForResult(intent, NATIVE_FILES);
        } catch (android.content.ActivityNotFoundException e) {
            filesSelectedNative(mAppContext, new String[0]);
        }
    }

    @Override
    protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        if (request != NATIVE_FILES || mAppContext == 0)
            return;
        java.util.ArrayList<String> paths = new java.util.ArrayList<>();
        try {
            if (result == RESULT_OK && data != null) {
                java.util.ArrayList<android.net.Uri> uris = new java.util.ArrayList<>();
                if (data.getClipData() != null) {
                    for (int i = 0; i < data.getClipData().getItemCount(); i++)
                        uris.add(data.getClipData().getItemAt(i).getUri());
                } else if (data.getData() != null)
                    uris.add(data.getData());
                for (android.net.Uri uri : uris)
                    paths.add((nativeDirectory ? AndroidStorage.directory(this, uri)
                                               : AndroidStorage.file(this, uri))
                                    .getAbsolutePath());
            }
        } catch (java.io.IOException e) {
            paths.clear();
            new android.app.AlertDialog.Builder(this)
                    .setMessage(e.getMessage())
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
        }
        filesSelectedNative(mAppContext, paths.toArray(new String[0]));
    }

    @Nullable private WindowSurfaceView mWindowSurfaceView = null;

    private native long initializeWindowedAppOnCreate(
            String windowedAppIdentifier, AssetManager assetManager);

    private native void onDestroyNative(long appContext);

    private native void onWindowSurfaceLayoutChange(
            long appContext, int left, int top, int right, int bottom);

    private native boolean onWindowSurfaceMotionEvent(long appContext, MotionEvent event);

    private native void onWindowSurfaceChanged(long appContext, Surface windowSurface);

    private native void paintWindow(long appContext, boolean forcePaint);

    protected abstract String getWindowedAppIdentifier();
    protected void sessionEvent(int event, String[] values) {}

    protected void setWindowSurfaceView(@Nullable final WindowSurfaceView windowSurfaceView) {
        if (mWindowSurfaceView == windowSurfaceView) {
            return;
        }

        // Detach from the old surface.
        if (mWindowSurfaceView != null) {
            mWindowSurfaceView.getHolder().removeCallback(mWindowSurfaceListener);
            mWindowSurfaceView.setOnTouchListener(null);
            mWindowSurfaceView.setOnGenericMotionListener(null);
            mWindowSurfaceView.removeOnLayoutChangeListener(mWindowSurfaceListener);
            mWindowSurfaceView = null;
            if (mAppContext != 0) {
                onWindowSurfaceChanged(mAppContext, null);
            }
        }

        if (windowSurfaceView == null) {
            return;
        }

        mWindowSurfaceView = windowSurfaceView;
        // FIXME(Triang3l): This doesn't work if the layout has already been performed.
        mWindowSurfaceView.addOnLayoutChangeListener(mWindowSurfaceListener);
        mWindowSurfaceView.setOnGenericMotionListener(mWindowSurfaceListener);
        mWindowSurfaceView.setOnTouchListener(mWindowSurfaceListener);
        final SurfaceHolder windowSurfaceHolder = mWindowSurfaceView.getHolder();
        windowSurfaceHolder.addCallback(mWindowSurfaceListener);
        // If setting after the creation of the surface.
        if (mAppContext != 0) {
            final Surface windowSurface = windowSurfaceHolder.getSurface();
            if (windowSurface != null) {
                onWindowSurfaceChanged(mAppContext, windowSurface);
            }
        }
    }

    public void onWindowSurfaceDraw(final boolean forcePaint) {
        if (mAppContext == 0) {
            return;
        }
        paintWindow(mAppContext, forcePaint);
    }

    // Used from the native WindowedAppContext. May be called from non-UI threads.
    @SuppressWarnings("UnusedDeclaration")
    protected void postInvalidateWindowSurface() {
        if (mWindowSurfaceView == null) {
            return;
        }
        mWindowSurfaceView.postInvalidate();
    }

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Bundle launch = getIntent().getBundleExtra(EXTRA_CVARS);
        String storage = launch == null ? null : launch.getString("storage_root");
        if (storage != null) {
            try {
                dataLock = new DataLock(new java.io.File(storage));
            } catch (java.io.IOException e) {
                android.widget.Toast
                        .makeText(this, R.string.data_busy, android.widget.Toast.LENGTH_LONG)
                        .show();
                finish();
                return;
            }
        }

        final String windowedAppIdentifier = getWindowedAppIdentifier();
        mAppContext = initializeWindowedAppOnCreate(windowedAppIdentifier, getAssets());
        if (mAppContext == 0) {
            finish();
            throw new XeniaRuntimeException(
                    "Error initializing the windowed app " + windowedAppIdentifier);
        }
    }

    @Override
    protected void onDestroy() {
        setWindowSurfaceView(null);
        if (mAppContext != 0) {
            onDestroyNative(mAppContext);
        }
        mAppContext = 0;
        if (dataLock != null) {
            try {
                dataLock.close();
            } catch (java.io.IOException ignored) {
            }
            dataLock = null;
        }
        super.onDestroy();
    }

    private class WindowSurfaceListener implements View.OnGenericMotionListener,
                                                   View.OnLayoutChangeListener,
                                                   View.OnTouchListener, SurfaceHolder.Callback2 {
        @Override
        public void onLayoutChange(final View v, final int left, final int top, final int right,
                final int bottom, final int oldLeft, final int oldTop, final int oldRight,
                final int oldBottom) {
            if (mAppContext != 0) {
                onWindowSurfaceLayoutChange(mAppContext, left, top, right, bottom);
            }
        }

        @Override
        public boolean onGenericMotion(final View view, final MotionEvent event) {
            if (mAppContext == 0) {
                return false;
            }
            return onWindowSurfaceMotionEvent(mAppContext, event);
        }

        @SuppressLint("ClickableViewAccessibility")
        @Override
        public boolean onTouch(final View view, final MotionEvent event) {
            if (mAppContext == 0) {
                return false;
            }
            return onWindowSurfaceMotionEvent(mAppContext, event);
        }

        @Override
        public void surfaceCreated(final SurfaceHolder holder) {
            if (mAppContext == 0) {
                return;
            }
            onWindowSurfaceChanged(mAppContext, holder.getSurface());
        }

        @Override
        public void surfaceChanged(
                final SurfaceHolder holder, final int format, final int width, final int height) {
            if (mAppContext == 0) {
                return;
            }
            onWindowSurfaceChanged(mAppContext, holder.getSurface());
        }

        @Override
        public void surfaceDestroyed(final SurfaceHolder holder) {
            if (mAppContext == 0) {
                return;
            }
            onWindowSurfaceChanged(mAppContext, null);
        }

        @Override
        public void surfaceRedrawNeeded(final SurfaceHolder holder) {
            onWindowSurfaceDraw(true);
        }
    }
}
