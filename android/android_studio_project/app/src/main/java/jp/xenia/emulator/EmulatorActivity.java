package jp.xenia.emulator;

import android.app.AlertDialog;
import android.hardware.input.InputManager;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.*;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** One game session, isolated from the library in the :emulation process. */
public class EmulatorActivity
        extends WindowedAppActivity implements InputManager.InputDeviceListener {
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private TextView status;
    private Button menu;
    private LinearLayout loading;
    private TouchController pad;
    private GuestKeyboard keyboard;
    private boolean touchVisible, textInput;
    private boolean pickingDisc;
    private boolean guideEnabled;
    private boolean started, running, busy = true, background, stopping;
    private String path;
    private int touchButtons;
    private final float[] touchAxes = new float[6];
    private final AndroidGamepad[] controllers = {
            new AndroidGamepad(), new AndroidGamepad(), new AndroidGamepad(), new AndroidGamepad()};
    private final android.os.Vibrator[] vibrators = new android.os.Vibrator[4];
    private final int[] vibrationValues = new int[4];
    private final int[] vibratorIds = {-2, -2, -2, -2};
    private android.content.SharedPreferences assignments;
    private final android.os.Handler uiHandler =
            new android.os.Handler(android.os.Looper.getMainLooper());
    private InputManager inputManager;
    private UsbPortal usbPortal;
    private DiscordBridge discord;
    private AudioManager audioManager;
    private AudioFocusRequest audioFocus;
    private boolean menuPaused, audioInterrupted;
    private AlertDialog stopConfirmation;

    private native int inputOptionsNative(long context);
    private native void controllerNative(long context, int slot, boolean keyboard, int subtype);
    private native void keyNative(
            long context, int key, int unicode, boolean down, int repeat, int modifiers);
    private native void focusNative(long context, boolean focused);
    private native int prepareNative(long context, String path);
    private native int launchNative(long context, String path);
    private native void pauseNative(long context, boolean paused);
    private native void inputNative(long context, int slot, boolean connected, int buttons, int lx,
            int ly, int rx, int ry, int lt, int rt);
    private native int[] vibrationNative(long context);
    private native void toolNative(long context, int tool);
    private native int uiStateNative(long context);
    private native void textNative(long context, String text, int key);
    private native int[] screenshotNative(long context);
    private native void discsNative(long context, String[] paths);
    private native void discSelectedNative(long context, String path);
    @Override
    protected String getWindowedAppIdentifier() {
        return "xenia";
    }

    @Override
    protected void onCreate(Bundle saved) {
        org.libsdl.app.SDL.setupJNI();
        org.libsdl.app.SDL.initialize();
        org.libsdl.app.SDL.setContext(this);
        setVolumeControlStream(android.media.AudioManager.STREAM_MUSIC);
        Bundle cvars = new Bundle();
        cvars.putString("storage_root", getFilesDir().getAbsolutePath());
        int scale = getIntent().getIntExtra("resolution", -1);
        cvars.putInt("android_resolution", scale);
        cvars.putInt("android_fps_limit", getIntent().getIntExtra("fps_limit", -1));
        String source = getIntent().getStringExtra("game_path");
        try {
            if (source != null && source.startsWith("content://"))
                source = AndroidStorage.file(this, Uri.parse(source)).getAbsolutePath();
        } catch (java.io.IOException ignored) {
            // The existing file validation below shows the launch error.
            source = null;
        }
        if (source != null)
            cvars.putString("android_game_path", source);
        cvars.putString("launch_module",
                getIntent().hasExtra("launch_module") ? getIntent().getStringExtra("launch_module")
                                                      : "");
        cvars.putString("launch_data",
                getIntent().hasExtra("launch_data") ? getIntent().getStringExtra("launch_data")
                                                    : "");
        cvars.putBoolean("log_append", getIntent().hasExtra("previous_pid"));
        cvars.putLong("launch_flags", getIntent().getLongExtra("launch_flags", 0));
        getIntent().putExtra(EXTRA_CVARS, cvars);
        super.onCreate(saved);
        if (getNativeAppContext() == 0)
            return;
        discord = new DiscordBridge(this);
        guideEnabled = inputOptionsNative(getNativeAppContext()) != 0;
        usbPortal = new UsbPortal(this, getNativeAppContext());
        assignments = getSharedPreferences("controllers", MODE_PRIVATE);
        inputManager = getSystemService(InputManager.class);
        inputManager.registerInputDeviceListener(this, null);
        for (int id : android.view.InputDevice.getDeviceIds()) onInputDeviceAdded(id);
        audioManager = getSystemService(AudioManager.class);
        audioFocus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                             .setAudioAttributes(new AudioAttributes.Builder()
                                             .setUsage(AudioAttributes.USAGE_GAME)
                                             .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                             .build())
                             .setWillPauseWhenDucked(true)
                             .setOnAudioFocusChangeListener(change -> {
                                 audioInterrupted = change != AudioManager.AUDIOFOCUS_GAIN;
                                 if (audioInterrupted)
                                     resetInput();
                                 updatePaused();
                             })
                             .build();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(android.graphics.Color.BLACK);
        WindowSurfaceView surface = new WindowSurfaceView(this, null);
        frame.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        pad = new TouchController(this, (buttons, lx, ly, rx, ry, lt, rt) -> {
            touchButtons = buttons;
            float[] axes = {lx, ly, rx, ry, lt, rt};
            System.arraycopy(axes, 0, touchAxes, 0, axes.length);
            sendInput();
        });
        touchVisible = getIntent().getBooleanExtra("touch_controls", true);
        pad.setVisibility(touchVisible ? View.VISIBLE : View.GONE);
        frame.addView(pad, new FrameLayout.LayoutParams(-1, -1));
        keyboard = new GuestKeyboard(
                this, (text, key) -> textNative(getNativeAppContext(), text, key));
        frame.addView(keyboard, new FrameLayout.LayoutParams(1, 1));
        menu = Ui.button(this, "Ⅱ  " + getString(R.string.menu), false);
        menu.setId(R.id.game_menu);
        FrameLayout.LayoutParams mp = new FrameLayout.LayoutParams(
                -2, Ui.dp(this, 44), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        mp.topMargin = Ui.dp(this, 12);
        frame.addView(menu, mp);
        menu.setOnClickListener(v -> showMenu());
        loading = Ui.column(this);
        loading.setGravity(Gravity.CENTER);
        loading.setBackgroundColor(0xF00F1319);
        loading.setId(R.id.game_loading);
        loading.addView(Ui.text(this, getIntent().getStringExtra("game_name"), 24, Ui.TEXT, true),
                Ui.space(this, -2, 18));
        ProgressBar progress = new ProgressBar(this);
        loading.addView(progress, new LinearLayout.LayoutParams(Ui.dp(this, 40), Ui.dp(this, 40)));
        status = Ui.text(this, getString(R.string.preparing_game), 14, Ui.MUTED, false);
        status.setGravity(Gravity.CENTER);
        loading.addView(status, Ui.space(this, -2, 12));
        frame.addView(loading, new FrameLayout.LayoutParams(-1, -1));
        setContentView(frame);
        setWindowSurfaceView(surface);
        getWindow().getInsetsController().hide(WindowInsets.Type.systemBars());
        getWindow().getInsetsController().setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        try {
            path = getIntent().getStringExtra("game_path");
            if (path == null || path.isEmpty())
                throw new java.io.IOException(getString(R.string.file_unavailable));
            if (path.startsWith("content://"))
                path = AndroidStorage.file(this, Uri.parse(path)).getAbsolutePath();
            if (!new java.io.File(path).canRead())
                throw new java.io.IOException(getString(R.string.file_unavailable));
        } catch (Exception e) {
            busy = false;
            fail(e.getMessage());
            return;
        }
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder holder) {}
            public void surfaceDestroyed(SurfaceHolder holder) {}
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                if (!started && width > 0 && height > 0) {
                    started = true;
                    surface.post(()
                                         -> new AndroidProfiles(EmulatorActivity.this,
                                                 getNativeAppContext(), worker,
                                                 EmulatorActivity.this::launch,
                                                 EmulatorActivity.this::stop)
                                                 .ensureSignedIn());
                }
            }
        });
    }
    private void launch() {
        if (isFinishing())
            return;
        android.util.Log.i("xenia", "Android surface ready; preparing title");
        status.setText(R.string.preparing_video);
        // A serial worker lets the UI service Vulkan's synchronous UI callbacks.
        // Presenter attachment itself must happen on the UI thread.
        final long context = getNativeAppContext();
        worker.execute(() -> {
            java.util.ArrayList<String> discs = new java.util.ArrayList<>();
            for (GameLibrary.Game game : new GameLibrary(this).games) {
                try {
                    discs.add(game.path.startsWith("content://")
                                    ? AndroidStorage.file(this, Uri.parse(game.path))
                                              .getAbsolutePath()
                                    : game.path);
                } catch (java.io.IOException ignored) {
                }
            }
            discsNative(context, discs.toArray(new String[0]));
            int prepared = prepareNative(context, path);
            runOnUiThread(() -> {
                if (prepared != 0) {
                    busy = false;
                    fail(getString(R.string.launch_error, prepared));
                    return;
                }
                status.setText(R.string.loading_game);
                worker.execute(() -> {
                    int result = launchNative(context, path);
                    runOnUiThread(() -> {
                        busy = false;
                        if (result != 0) {
                            fail(getString(R.string.launch_error, result));
                            return;
                        }
                        configureControllers();
                        running = true;
                        loading.setVisibility(View.GONE);
                        updatePaused();
                    });
                });
            });
        });
    }
    private void fail(String message) {
        android.util.Log.e("xenia", "Android launch error: " + message);
        status.setText(message);
        new AlertDialog.Builder(this)
                .setTitle(R.string.cannot_launch)
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton(R.string.back_library, (d, w) -> finish())
                .show();
    }
    private void updatePaused() {
        boolean value = background || menuPaused || audioInterrupted;
        if (!running || stopping)
            return;
        long context = getNativeAppContext();
        focusNative(context, !value);
        worker.execute(() -> pauseNative(context, value));
    }
    private void showMenu() {
        if (busy) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.close_session)
                    .setMessage(R.string.interrupt_loading)
                    .setNegativeButton(android.R.string.cancel, null)
                    .setPositiveButton(R.string.stop_game, (d, w) -> stop())
                    .show();
            return;
        }
        if (!running || stopping)
            return;
        resetInput();
        menuPaused = true;
        updatePaused();
        java.util.ArrayList<String> labels = new java.util.ArrayList<>();
        java.util.ArrayList<Runnable> actions = new java.util.ArrayList<>();
        java.util.function.BiConsumer<Integer, Runnable> add = (label, action) -> {
            labels.add(getString(label));
            actions.add(action);
        };
        add.accept(R.string.resume, () -> {});
        add.accept(R.string.toggle_controls, () -> touchVisible = !touchVisible);
        add.accept(R.string.game_options,
                ()
                        -> startActivity(
                                new android.content.Intent(this, GameSettingsActivity.class)
                                        .putExtra("game_path", path)
                                        .putExtra("title_id", (int) getIntent().getLongExtra("title_id", 0))
                                        .putExtra("game_name",
                                                getIntent().getStringExtra("game_name"))));
        add.accept(R.string.controllers, this::showControllers);
        int[] panels = {R.string.postprocessing, R.string.performance, R.string.debug_tools,
                R.string.audio, R.string.profiles};
        for (int i = 0; i < panels.length; i++) {
            final int tool = i;
            add.accept(panels[i], () -> toolNative(getNativeAppContext(), tool));
        }
        add.accept(R.string.time_scale, this::showTimeScale);
        add.accept(R.string.toggle_vibration, () -> toolNative(getNativeAppContext(), 11));
        add.accept(R.string.screenshot, this::screenshot);
        add.accept(R.string.clear_gpu_cache, () -> toolNative(getNativeAppContext(), 5));
        int runtimeState = uiStateNative(getNativeAppContext());
        if ((runtimeState & 8) != 0)
            add.accept(R.string.profiler, () -> toolNative(getNativeAppContext(), 6));
        add.accept(R.string.stop_game, this::confirmStop);
        String title = getIntent().getStringExtra("game_name");
        if ((runtimeState & 16) != 0)
            title += "\n" + getString(R.string.patches_applied);
        if ((runtimeState & 32) != 0)
            title += "\n" + getString(R.string.plugins_loaded);
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setItems(labels.toArray(new String[0]), (d, which) -> actions.get(which).run())
                .setOnDismissListener(d -> {
                    menuPaused = stopConfirmation != null && stopConfirmation.isShowing();
                    updatePaused();
                })
                .show();
    }
    private void showTimeScale() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.time_scale)
                .setItems(new String[] {getString(R.string.time_half),
                                  getString(R.string.time_double), getString(R.string.time_reset)},
                        (d, which) -> toolNative(getNativeAppContext(), 8 + which))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    @Override
    protected void sessionEvent(int event, String[] values) {
        if (event == 0 || event == 1) {
            if (discord != null)
                discord.close();
            stopping = true;
            android.content.Intent next =
                    new android.content.Intent(this, SessionActivity.class)
                            .putExtra("previous_pid", android.os.Process.myPid());
            if (event == 0) {
                next.putExtra("game_path", values[0])
                        .putExtra("launch_module", values[1])
                        .putExtra("launch_flags", Long.parseLong(values[2]))
                        .putExtra("launch_data", values[3]);
            }
            startActivity(next);
            finish(); // Remove the old singleTask record before the process exits.
        } else if (event == 2) {
            if (discord != null)
                discord.playing(values[0]);
            getIntent().putExtra("game_name", values[0]);
            getIntent().putExtra("title_id", Long.parseLong(values[1]));
            if (values.length > 2 && !values[2].isEmpty()) {
                path = values[2];
                getIntent().putExtra("game_path", path);
            }
            if (assignments != null)
                configureControllers();
            if (usbPortal != null)
                usbPortal.rebind();
            if (status != null)
                status.setText(values[0]);
        } else if (event == 4) {
            int tool = Integer.parseInt(values[0]);
            String label = tool <= 10 ? getString(R.string.time_scale)
                    : tool == 11      ? getString(R.string.toggle_vibration)
                    : tool == 13      ? "readback_resolve"
                    : tool == 14      ? "log_level"
                                      : "clear_memory_page_state";
            Toast.makeText(this, label + ": " + values[1], Toast.LENGTH_SHORT).show();
        } else if (event == 5) {
            boolean initializing = "1".equals(values[0]);
            if (status != null)
                status.setText(initializing ? R.string.preloading_shaders : R.string.loading_game);
            if (loading != null)
                loading.setVisibility(initializing || !running ? View.VISIBLE : View.GONE);
        } else if (event == 3) {
            getIntent().putExtra("disc_number", Integer.parseInt(values[0]));
            Toast.makeText(this, getString(R.string.disc_label, Integer.parseInt(values[0])),
                         Toast.LENGTH_SHORT)
                    .show();
        }
    }
    private void confirmStop() {
        stopConfirmation = new AlertDialog.Builder(this)
                .setTitle(R.string.stop_game)
                .setMessage(R.string.stop_confirmation)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.stop_game, (d, w) -> stop())
                .setOnDismissListener(d -> {
                    stopConfirmation = null;
                    menuPaused = false;
                    updatePaused();
                })
                .show();
    }
    private void stop() {
        if (discord != null)
            discord.close();
        stopping = true;
        pad.reset();
        // TerminateTitle exits the process without returning. Navigate first,
        // including when Android has discarded the library activity; onDestroy
        // then ends this isolated emulation process.
        startActivity(new android.content.Intent(this, LauncherActivity.class));
        finish();
    }
    private void sendInput() {
        if (stopping || getNativeAppContext() == 0)
            return;
        for (int slot = 0; slot < controllers.length; ++slot) {
            AndroidGamepad controller = controllers[slot];
            int[] axes = new int[6];
            for (int i = 0; i < axes.length; ++i)
                axes[i] = Math.round(
                        TouchController.clamp((slot == 0 ? touchAxes[i] : 0) + controller.axis(i))
                        * (i < 4 ? 32767 : 255));
            inputNative(getNativeAppContext(), slot,
                    !"keyboard".equals(assignments.getString("slot" + slot, ""))
                            && !"disconnected".equals(assignments.getString("slot" + slot, ""))
                            && (slot == 0 || controller.deviceId != -1),
                    (slot == 0 ? touchButtons : 0) | controller.buttons | controller.hatButtons,
                    axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
        }
    }
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (inputActive()) {
            AndroidGamepad pad = controllerFor(event.getDeviceId());
            int code = pad == null || pad.mappings == null ? event.getKeyCode()
                                                           : pad.mappings.key(event.getKeyCode());
            if (pad != null && code == KeyEvent.KEYCODE_BUTTON_MODE) {
                if (AndroidGamepad.opensMenu(code, event.getAction(), event.getRepeatCount(), guideEnabled))
                    showMenu();
                return true;
            }
        }
        if (textInput && keyboard != null && keyboard.key(event))
            return true;
        AndroidGamepad controller = controllerFor(event.getDeviceId());
        if (inputActive() && controller != null && controller.key(event)) {
            sendInput();
            return true;
        }
        if (inputActive() && controller == null && !textInput) {
            int key = KeyboardKeys.virtualKey(event.getKeyCode());
            if (key != 0) {
                if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0
                        && keyboardShortcut(key))
                    return true;
                keyNative(getNativeAppContext(), key, event.getUnicodeChar(),
                        event.getAction() == KeyEvent.ACTION_DOWN, event.getRepeatCount(),
                        (event.isShiftPressed() ? 1 : 0) | (event.isCtrlPressed() ? 2 : 0)
                                | (event.isAltPressed() ? 4 : 0) | (event.isMetaPressed() ? 8 : 0));
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        AndroidGamepad controller = controllerFor(event.getDeviceId());
        if (inputActive() && controller != null && controller.motion(event)) {
            sendInput();
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }
    private boolean inputActive() {
        return running && !stopping && !background && !menuPaused && !audioInterrupted;
    }
    private boolean keyboardShortcut(int key) {
        if (key == 27) {
            onBackPressed();
            return true;
        }
        if (key == 123) {
            screenshot();
            return true;
        }
        int tool = key == 106              ? 10
                : key == 109               ? 8
                : key == 107               ? 9
                : key == 114               ? 6
                : key == 115               ? 12
                : key == 116               ? 5
                : key >= 117 && key <= 119 ? key - 117
                                           : -1;
        if (tool < 0)
            return false;
        toolNative(getNativeAppContext(), tool);
        return true;
    }
    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (getNativeAppContext() != 0)
            focusNative(getNativeAppContext(), focused && inputActive());
    }
    private void configureControllers() {
        for (AndroidGamepad controller : controllers) {
            android.view.InputDevice device = controller.deviceId < 0
                    ? null
                    : android.view.InputDevice.getDevice(controller.deviceId);
            controller.mappings = device == null
                    ? null
                    : new ControllerMappings(assignments, device.getDescriptor());
        }
        for (int slot = 0; slot < 4; slot++)
            controllerNative(getNativeAppContext(), slot,
                    "keyboard".equals(assignments.getString("slot" + slot, "")),
                    assignments.getInt("type" + slot, 0));
    }
    private void resetInput() {
        if (getNativeAppContext() != 0)
            focusNative(getNativeAppContext(), false);
        for (AndroidGamepad controller : controllers) {
            int id = controller.deviceId;
            controller.clear();
            controller.deviceId = id;
        }
        stopVibration();
        if (pad != null)
            pad.reset();
        sendInput();
    }
    @Override
    public void onInputDeviceAdded(int deviceId) {
        AndroidGamepad controller = controllerFor(deviceId);
        if (controller != null) {
            controller.deviceId = deviceId;
            sendInput();
            configureControllers();
        }
    }
    @Override
    public void onInputDeviceChanged(int deviceId) {
        onInputDeviceRemoved(deviceId);
        onInputDeviceAdded(deviceId);
    }
    @Override
    public void onInputDeviceRemoved(int deviceId) {
        for (AndroidGamepad controller : controllers)
            if (controller.deviceId == deviceId) {
                controller.clear();
                sendInput();
                configureControllers();
            }
    }
    private AndroidGamepad controllerFor(int id) {
        android.view.InputDevice device = android.view.InputDevice.getDevice(id);
        if (device == null
                || !(device.supportsSource(android.view.InputDevice.SOURCE_GAMEPAD)
                        || device.supportsSource(android.view.InputDevice.SOURCE_JOYSTICK)))
            return null;
        for (AndroidGamepad controller : controllers)
            if (controller.deviceId == id)
                return controller;
        String descriptor = device.getDescriptor();
        for (int slot = 0; slot < 4; slot++) {
            String selected = assignments.getString("slot" + slot, null);
            if (descriptor.equals(selected)) {
                controllers[slot].clear();
                controllers[slot].deviceId = id;
                return controllers[slot];
            }
        }
        for (int slot = 0; slot < 4; slot++)
            if (!assignments.contains("slot" + slot) && controllers[slot].deviceId == -1)
                return controllers[slot];
        return null;
    }
    private void showControllers() {
        String[] slots = new String[4];
        for (int i = 0; i < 4; i++) {
            android.view.InputDevice device = controllers[i].deviceId < 0
                    ? null
                    : android.view.InputDevice.getDevice(controllers[i].deviceId);
            slots[i] = getString(R.string.player_number, i + 1) + " · "
                    + ("disconnected".equals(assignments.getString("slot" + i, ""))
                                    ? getString(R.string.disconnect_controller)
                                    : "virtual".equals(assignments.getString("slot" + i, ""))
                                    ? getString(R.string.virtual_controller)
                                    : "keyboard".equals(assignments.getString("slot" + i, ""))
                                    ? getString(R.string.physical_keyboard)
                                    : device == null ? getString(R.string.no_controller)
                                                     : device.getName());
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.controllers)
                .setItems(slots,
                        (dialog, slot) -> {
                            java.util.ArrayList<Integer> ids = new java.util.ArrayList<>();
                            java.util.ArrayList<String> names = new java.util.ArrayList<>();
                            ids.add(-2);
                            names.add(getString(R.string.automatic_controller));
                            ids.add(-3);
                            names.add(getString(R.string.physical_keyboard));
                            if (slot == 0) {
                                ids.add(-1);
                                names.add(getString(R.string.virtual_controller));
                            }
                            ids.add(-4);
                            names.add(getString(R.string.disconnect_controller));
                            for (int id : android.view.InputDevice.getDeviceIds()) {
                                android.view.InputDevice device =
                                        android.view.InputDevice.getDevice(id);
                                if (device != null
                                        && (device.supportsSource(
                                                    android.view.InputDevice.SOURCE_GAMEPAD)
                                                || device.supportsSource(android.view.InputDevice
                                                                .SOURCE_JOYSTICK))) {
                                    ids.add(id);
                                    names.add(device.getName());
                                }
                            }
                            new AlertDialog.Builder(this)
                                    .setTitle(slots[slot])
                                    .setItems(names.toArray(new String[0]),
                                            (d, which) -> {
                                                resetInput();
                                                int id = ids.get(which);
                                                android.content.SharedPreferences.Editor edit =
                                                        assignments.edit();
                                                String descriptor = id >= 0
                                                        ? android.view.InputDevice.getDevice(id)
                                                                  .getDescriptor()
                                                        : id == -3 ? "keyboard"
                                                        : id == -4 ? "disconnected"
                                                                   : "virtual";
                                                for (int i = 0; i < 4; i++)
                                                    if (i != slot && (id >= 0 || id == -3)
                                                            && (controllers[i].deviceId == id
                                                                    || descriptor.equals(
                                                                            assignments.getString(
                                                                                    "slot" + i,
                                                                                    null)))) {
                                                        controllers[i].clear();
                                                        edit.putString("slot" + i, "");
                                                    }
                                                if (id == -2)
                                                    edit.remove("slot" + slot);
                                                else
                                                    edit.putString("slot" + slot, descriptor);
                                                edit.apply();
                                                controllers[slot].clear();
                                                controllers[slot].deviceId = id >= 0 ? id : -1;
                                                sendInput();
                                                configureControllers();
                                            })
                                    .setPositiveButton(R.string.remap_controller,
                                            (d, w)
                                                    -> ControllerMappings.show(this, assignments,
                                                            controllers[slot].deviceId,
                                                            this::configureControllers))
                                    .setNeutralButton(R.string.controller_type,
                                            (d, w) -> showControllerType(slot))
                                    .setNegativeButton(android.R.string.cancel, null)
                                    .show();
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
    private void showControllerType(int slot) {
        int[] types = {0, 1, 2, 3, 4, 5, 6, 8, 19};
        String[] names = getResources().getStringArray(R.array.controller_types);
        int selected = 0;
        for (int i = 0; i < types.length; i++)
            if (types[i] == assignments.getInt("type" + slot, 0))
                selected = i;
        new AlertDialog.Builder(this)
                .setTitle(R.string.controller_type)
                .setSingleChoiceItems(names, selected,
                        (d, which) -> {
                            assignments.edit().putInt("type" + slot, types[which]).apply();
                            configureControllers();
                            d.dismiss();
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
    private void stopVibration() {
        for (int i = 0; i < 4; i++) {
            if (vibrators[i] != null)
                vibrators[i].cancel();
            vibrators[i] = null;
            vibratorIds[i] = -2;
            vibrationValues[i] = 0;
        }
    }
    private final Runnable pollUi = new Runnable() {
        @Override
        public void run() {
            if (discord != null && !stopping)
                discord.poll();
            if (running && !stopping && !background) {
                int state = uiStateNative(getNativeAppContext());
                if ((state & 4) != 0 && !pickingDisc) {
                    pickingDisc = true;
                    startActivityForResult(
                            new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT)
                                    .setType("*/*")
                                    .addCategory(android.content.Intent.CATEGORY_OPENABLE)
                                    .addFlags(
                                            android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION),
                            20);
                }
                boolean virtual = !"disconnected".equals(assignments.getString("slot0", ""))
                        && !"keyboard".equals(assignments.getString("slot0", ""));
                pad.setVisibility(
                        virtual && touchVisible && (state & 1) == 0 ? View.VISIBLE : View.GONE);
                menu.setVisibility((state & 1) == 0 ? View.VISIBLE : View.GONE);
                boolean wantsText = (state & 2) != 0;
                if (wantsText != textInput) {
                    textInput = wantsText;
                    keyboard.show(wantsText);
                }
                int[] values = vibrationNative(getNativeAppContext());
                for (int i = 0; i < 4; i++) {
                    int deviceId = controllers[i].deviceId;
                    if (vibratorIds[i] != deviceId) {
                        if (vibrators[i] != null)
                            vibrators[i].cancel();
                        android.view.InputDevice device =
                                android.view.InputDevice.getDevice(deviceId);
                        vibrators[i] = device != null ? device.getVibrator()
                                : i == 0              ? getSystemService(android.os.Vibrator.class)
                                                      : null;
                        vibratorIds[i] = deviceId;
                        vibrationValues[i] = 0;
                    }
                    android.os.Vibrator vibrator = vibrators[i];
                    int value = inputActive() ? values[i] : 0;
                    if (vibrator != vibrators[i] || value != vibrationValues[i]) {
                        if (vibrators[i] != null)
                            vibrators[i].cancel();
                        vibrators[i] = vibrator;
                        vibrationValues[i] = value;
                        if (value != 0 && vibrator != null && vibrator.hasVibrator()) {
                            int amplitude = Math.max(value & 65535, value >>> 16) * 254 / 65535 + 1;
                            vibrator.vibrate(android.os.VibrationEffect.createWaveform(
                                    new long[] {100}, new int[] {amplitude}, 0));
                        }
                    }
                }
            }
            uiHandler.postDelayed(this, 50);
        }
    };
    private void screenshot() {
        worker.execute(() -> {
            try {
                int[] image = screenshotNative(getNativeAppContext());
                if (image == null)
                    throw new java.io.IOException(getString(R.string.screenshot_failed));
                android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(image, 2,
                        image[0], image[0], image[1], android.graphics.Bitmap.Config.ARGB_8888);
                java.io.File folder = new java.io.File(getFilesDir(), "screenshots");
                android.util.AtomicFile file = new android.util.AtomicFile(
                        new java.io.File(folder, "Xenia-" + System.currentTimeMillis() + ".png"));
                java.io.FileOutputStream output = null;
                try {
                    output = file.startWrite();
                    if (!bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, output))
                        throw new java.io.IOException(getString(R.string.screenshot_failed));
                    file.finishWrite(output);
                } catch (Exception e) {
                    file.failWrite(output);
                    throw e;
                } finally {
                    bitmap.recycle();
                }
                runOnUiThread(()
                                      -> Toast.makeText(this, R.string.screenshot_saved,
                                                      Toast.LENGTH_LONG)
                                              .show());
            } catch (Exception e) {
                runOnUiThread(
                        ()
                                -> Toast.makeText(this,
                                                getString(R.string.operation_error, e.getMessage()),
                                                Toast.LENGTH_LONG)
                                        .show());
            }
        });
    }
    @Override
    protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        if (request != 20)
            return;
        String selected = null;
        try {
            if (result == RESULT_OK && data != null && data.getData() != null) {
                selected = AndroidStorage.file(this, data.getData()).getAbsolutePath();
                getContentResolver().call(
                        getPackageName() + ".documents", "register_disc", selected, null);
            }
        } catch (java.io.IOException | IllegalArgumentException e) {
            selected = null;
            Toast.makeText(this, e.getMessage(), Toast.LENGTH_LONG).show();
        }
        discSelectedNative(getNativeAppContext(), selected);
        pickingDisc = false;
    }
    @Override
    public void onBackPressed() {
        if (running && !stopping && (uiStateNative(getNativeAppContext()) & 1) != 0) {
            toolNative(getNativeAppContext(), 7);
            return;
        }
        showMenu();
    }
    @Override
    protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        if (!java.util.Objects.equals(
                    intent.getStringExtra("game_path"), getIntent().getStringExtra("game_path")))
            Toast.makeText(this, R.string.close_current_first, Toast.LENGTH_LONG).show();
    }
    @Override
    protected void onPause() {
        background = true;
        uiHandler.removeCallbacks(pollUi);
        resetInput();
        updatePaused();
        if (audioManager != null)
            audioManager.abandonAudioFocusRequest(audioFocus);
        super.onPause();
    }
    @Override
    protected void onResume() {
        super.onResume();
        background = false;
        if (audioManager != null) {
            audioInterrupted = audioManager.requestAudioFocus(audioFocus)
                    != AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        }
        updatePaused();
        uiHandler.removeCallbacks(pollUi);
        uiHandler.post(pollUi);
    }
    @Override
    protected void onDestroy() {
        uiHandler.removeCallbacks(pollUi);
        if (discord != null)
            discord.close();
        if (usbPortal != null)
            usbPortal.close();
        stopVibration();
        if (inputManager != null)
            inputManager.unregisterInputDeviceListener(this);
        if (audioManager != null)
            audioManager.abandonAudioFocusRequest(audioFocus);
        // Android may destroy an activity while native loading is in flight.
        // This process contains only this game: avoid freeing its context under
        // the worker or blocking the UI on a worker waiting for a UI callback.
        if (busy || running) {
            android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }
        worker.shutdown();
        super.onDestroy();
        // Like the desktop per-title process, each session starts with clean
        // emulator globals. The library lives in a different process.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
