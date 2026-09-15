package jp.xenia.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.text.InputFilter;
import android.text.InputType;
import android.view.View;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.concurrent.ExecutorService;

/** Native Android entry to the desktop ProfileManager, before any guest runs. */
final class AndroidProfiles {
    private final Activity activity;
    private final long context;
    private final ExecutorService worker;
    private final Runnable ready, cancel;

    private static native String[] listNative(long context);
    private static native boolean migrationNative(long context);
    private static native int createNative(long context, String gamertag);
    private static native boolean loginNative(long context, long xuid);

    AndroidProfiles(Activity activity, long context, ExecutorService worker, Runnable ready,
            Runnable cancel) {
        this.activity = activity;
        this.context = context;
        this.worker = worker;
        this.ready = ready;
        this.cancel = cancel;
    }

    void ensureSignedIn() {
        worker.execute(() -> {
            String[] profiles = listNative(context);
            boolean migration =
                    profiles != null && profiles.length == 0 && migrationNative(context);
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed())
                    return;
                if (profiles == null) {
                    ready.run();
                } else if (profiles.length == 0) {
                    create(migration);
                } else {
                    select(profiles);
                }
            });
        });
    }

    private void select(String[] profiles) {
        String[] labels = new String[profiles.length / 2];
        for (int i = 0; i < labels.length; ++i)
            labels[i] = profiles[i * 2 + 1].isEmpty() ? profiles[i * 2] : profiles[i * 2 + 1];
        new AlertDialog.Builder(activity)
                .setTitle(R.string.profile_select)
                .setItems(labels, (d, which) -> worker.execute(() -> {
                    boolean loggedIn =
                            loginNative(context, Long.parseUnsignedLong(profiles[which * 2], 16));
                    activity.runOnUiThread(() -> {
                        if (activity.isFinishing() || activity.isDestroyed())
                            return;
                        if (loggedIn) {
                            ready.run();
                        } else {
                            new AlertDialog.Builder(activity)
                                    .setMessage(R.string.profile_error)
                                    .setPositiveButton(android.R.string.ok,
                                            (dialog, button) -> ensureSignedIn())
                                    .setOnCancelListener(dialog -> cancel.run())
                                    .show();
                        }
                    });
                }))
                .setPositiveButton(R.string.profile_create, (d, w) -> create(false))
                .setNegativeButton(R.string.back_library, (d, w) -> cancel.run())
                .setOnCancelListener(d -> cancel.run())
                .show();
    }

    private void create(boolean migration) {
        // Large landscape IMEs can leave no room for the name field. The game
        // keeps its own orientation; only this one-time form uses portrait.
        int previousOrientation = activity.getRequestedOrientation();
        activity.setRequestedOrientation(
                android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
        Runnable restore = () -> activity.setRequestedOrientation(previousOrientation);
        Runnable cancelled = () -> {
            restore.run();
            cancel.run();
        };
        LinearLayout form = Ui.column(activity);
        int padding = Ui.dp(activity, 24);
        form.setPadding(padding, 0, padding, 0);
        form.addView(Ui.text(activity,
                activity.getString(
                        migration ? R.string.profile_migration : R.string.profile_explanation),
                14, Ui.TEXT, false));
        EditText name = new EditText(activity);
        name.setId(R.id.profile_gamertag);
        name.setHint(R.string.profile_gamertag);
        name.setSingleLine(true);
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        name.setImeOptions(android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        name.setFilters(new InputFilter[] {new InputFilter.LengthFilter(15)});
        form.addView(name);
        TextView error = Ui.text(activity, "", 14, 0xFFFFB4AB, false);
        error.setVisibility(View.GONE);
        form.addView(error);
        AlertDialog dialog =
                new AlertDialog.Builder(activity)
                        .setTitle(R.string.profile_create)
                        .setView(form)
                        .setPositiveButton(R.string.profile_create, null)
                        .setNegativeButton(android.R.string.cancel, (d, w) -> cancelled.run())
                        .setOnCancelListener(d -> cancelled.run())
                        .create();
        dialog.setOnShowListener(d -> {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                String gamertag = name.getText().toString();
                // Validation and persistence belong to the same core as desktop.
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
                dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(false);
                dialog.setCancelable(false);
                error.setVisibility(View.GONE);
                worker.execute(() -> {
                    int result = createNative(context, gamertag);
                    activity.runOnUiThread(() -> {
                        if (activity.isFinishing() || activity.isDestroyed())
                            return;
                        if (result == 0) {
                            dialog.dismiss();
                            restore.run();
                            ready.run();
                            return;
                        }
                        error.setText(
                                activity.getString(result == 0xC000000D ? R.string.profile_invalid
                                                                        : R.string.profile_error));
                        error.setVisibility(View.VISIBLE);
                        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(true);
                        dialog.setCancelable(true);
                    });
                });
            });
            name.requestFocus();
            dialog.getWindow().setSoftInputMode(
                    WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                    | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        });
        dialog.show();
    }
}
