package jp.xenia.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.Spinner;
import java.util.ArrayList;

/** Small per-device overrides on top of Android's standardized input mapping. */
final class ControllerMappings {
    static final int[] KEYS = {
            96, 97, 99, 100, 19, 20, 21, 22, 108, 109, 106, 107, 102, 103, 104, 105};
    static final int[] AXES = {0, 1, 11, 14, 17, 18};
    private final android.util.SparseIntArray keys = new android.util.SparseIntArray();
    private final int[] axes = AXES.clone();
    private final boolean[] inverted = new boolean[6];
    ControllerMappings(SharedPreferences prefs, String descriptor) {
        String prefix = "mapping." + descriptor + ".";
        for (java.util.Map.Entry<String, ?> item : prefs.getAll().entrySet()) {
            if (!item.getKey().startsWith(prefix + "key."))
                continue;
            try {
                keys.put(Integer.parseInt(item.getKey().substring((prefix + "key.").length())),
                        (Integer) item.getValue());
            } catch (RuntimeException ignored) {
            }
        }
        for (int i = 0; i < 6; i++) {
            axes[i] = prefs.getInt(prefix + "axis." + i, AXES[i]);
            inverted[i] = prefs.getBoolean(prefix + "invert." + i, false);
        }
    }
    int key(int code) {
        return keys.get(code, code);
    }
    int axis(int index) {
        return axes[index];
    }
    float sign(int index) {
        return inverted[index] ? -1 : 1;
    }
    static void show(Activity activity, SharedPreferences prefs, int deviceId, Runnable changed) {
        InputDevice device = InputDevice.getDevice(deviceId);
        if (deviceId < 0 || device == null)
            return;
        String prefix = "mapping." + device.getDescriptor() + ".";
        String[] names = {"A", "B", "X", "Y", "↑", "↓", "←", "→", "Start", "Back", "LS", "RS", "LB",
                "RB", "LT", "RT", "LX", "LY", "RX", "RY", "LT axis", "RT axis"};
        new AlertDialog.Builder(activity)
                .setTitle(R.string.remap_controller)
                .setItems(names,
                        (dialog, index) -> {
                            if (index < KEYS.length) {
                                AlertDialog capture =
                                        new AlertDialog.Builder(activity)
                                                .setTitle(names[index])
                                                .setMessage(R.string.press_controller_button)
                                                .setNegativeButton(android.R.string.cancel, null)
                                                .create();
                                capture.setOnKeyListener((d, code, event) -> {
                                    if (event.getDeviceId() != deviceId
                                            || event.getAction() != KeyEvent.ACTION_DOWN
                                            || code == KeyEvent.KEYCODE_BACK)
                                        return false;
                                    prefs.edit()
                                            .putInt(prefix + "key." + code, KEYS[index])
                                            .apply();
                                    changed.run();
                                    capture.dismiss();
                                    return true;
                                });
                                capture.show();
                            } else {
                                int channel = index - KEYS.length;
                                ArrayList<Integer> ids = new ArrayList<>();
                                ArrayList<String> labels = new ArrayList<>();
                                for (InputDevice.MotionRange range : device.getMotionRanges()) {
                                    if (!ids.contains(range.getAxis())) {
                                        ids.add(range.getAxis());
                                        labels.add(MotionEvent.axisToString(range.getAxis()));
                                    }
                                }
                                if (ids.isEmpty())
                                    return;
                                LinearLayout form = Ui.column(activity);
                                Spinner choice = new Spinner(activity);
                                choice.setAdapter(new android.widget.ArrayAdapter<>(activity,
                                        android.R.layout.simple_spinner_dropdown_item, labels));
                                ControllerMappings current =
                                        new ControllerMappings(prefs, device.getDescriptor());
                                choice.setSelection(
                                        Math.max(0, ids.indexOf(current.axis(channel))));
                                CheckBox invert = new CheckBox(activity);
                                invert.setText(R.string.invert_axis);
                                invert.setChecked(current.sign(channel) < 0);
                                form.addView(choice);
                                form.addView(invert);
                                new AlertDialog.Builder(activity)
                                        .setTitle(names[index])
                                        .setView(form)
                                        .setPositiveButton(R.string.options_save,
                                                (d, w) -> {
                                                    prefs.edit()
                                                            .putInt(prefix + "axis." + channel,
                                                                    ids.get(choice.getSelectedItemPosition()))
                                                            .putBoolean(
                                                                    prefix + "invert." + channel,
                                                                    invert.isChecked())
                                                            .apply();
                                                    changed.run();
                                                })
                                        .setNegativeButton(android.R.string.cancel, null)
                                        .show();
                            }
                        })
                .setNeutralButton(R.string.reset_mappings,
                        (d, w) -> {
                            SharedPreferences.Editor edit = prefs.edit();
                            for (String key : prefs.getAll().keySet())
                                if (key.startsWith(prefix))
                                    edit.remove(key);
                            edit.apply();
                            changed.run();
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
}
