package jp.xenia.emulator;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.*;
import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Desktop management services in an isolated process, without booting a game/GPU. */
public class ToolsActivity extends WindowedAppActivity {
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private LinearLayout entries;
    private TextView status;
    private String page;
    private int title, version;
    private long xuid;
    private boolean busy;
    @Override
    protected String getWindowedAppIdentifier() {
        return "xenia";
    }
    @Override
    protected void onCreate(Bundle saved) {
        Bundle cvars = new Bundle();
        cvars.putString("storage_root", getFilesDir().getAbsolutePath());
        getIntent().putExtra(EXTRA_CVARS, cvars);
        super.onCreate(saved);
        if (getNativeAppContext() == 0)
            return;
        page = getIntent().getStringExtra("page");
        if (page == null)
            page = "profiles";
        if (saved != null) {
            page = saved.getString("page", page);
            xuid = saved.getLong("xuid");
        }
        LinearLayout root = Ui.column(this);
        root.setBackgroundColor(Ui.BG);
        Ui.applyInsets(root, 16, 8);
        setContentView(root);
        Button back = Ui.button(this, "‹  " + getString(R.string.back_library), false);
        back.setOnClickListener(v -> onBackPressed());
        root.addView(back, Ui.space(this, 48, 8));
        status = Ui.text(this, getString(R.string.info_loading), 14, Ui.MUTED, false);
        root.addView(status, Ui.space(this, -2, 8));
        ScrollView scroll = new ScrollView(this);
        entries = Ui.column(this);
        scroll.addView(entries);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        busy = true;
        worker.execute(() -> {
            try {
                String path = getIntent().getStringExtra("game_path");
                title = getIntent().getIntExtra("title_id", 0);
                version = getIntent().getIntExtra("version", 0);
                if (path != null && title == 0) {
                    File source = path.startsWith("content://")
                            ? AndroidStorage.file(this, Uri.parse(path))
                            : new File(path);
                    int[] info = GameIcons.metadataNative(source.getAbsolutePath());
                    if (info == null || info.length < 3 || info[0] == 0)
                        throw new java.io.IOException(getString(R.string.options_no_title));
                    title = info[0];
                    version = info[2];
                }
                load();
            } catch (Exception e) {
                failed(e);
            }
        });
    }
    private void load() throws java.io.IOException {
        String[][] data = page.equals("install")
                ? new String[0][]
                : AndroidTools.listNative(getNativeAppContext(), page, title, xuid);
        runOnUiThread(() -> {
            busy = false;
            if (isFinishing() || isDestroyed())
                return;
            entries.removeAllViews();
            status.setText(label(page));
            if (page.equals("profiles"))
                button(getString(R.string.profile_create), () -> {
                    if (data.length == 0)
                        new AndroidProfiles(
                                this, getNativeAppContext(), worker, this::refresh, () -> {})
                                .ensureSignedIn();
                    else
                        edit("create", "", "", getString(R.string.profile_gamertag));
                });
            if (page.equals("content") || page.equals("install")) {
                entries.addView(Ui.text(this, getString(R.string.content_link_explanation), 14,
                                        Ui.MUTED, false),
                        Ui.space(this, -2, 12));
                button(getString(R.string.add_content), this::pickContent);
            }
            if (page.equals("saves") || page.equals("content"))
                button(getString(R.string.manage_files), () -> {
                    XeniaDocumentsProvider.open(this, "content");
                    finish();
                });
            if (data.length == 0)
                entries.addView(
                        Ui.text(this, getString(R.string.nothing_here), 14, Ui.MUTED, false));
            for (String[] row : data) {
                if (page.equals("profiles")) {
                    String assignment = row[2].equals("0")
                            ? getString(R.string.signed_out)
                            : getString(R.string.player_number, Integer.parseInt(row[2]));
                    button(row[1] + " · " + assignment, () -> profileMenu(row));
                } else if (page.equals("profile")) {
                    if (row[0].equals("icon")) {
                        button(getString(R.string.profile_image),
                                ()
                                        -> new AlertDialog.Builder(this)
                                                .setTitle(R.string.profile_image)
                                                .setItems(
                                                        new String[] {
                                                                getString(R.string.choose_image),
                                                                getString(R.string.clear_image)},
                                                        (d, index) -> {
                                                            if (index == 0)
                                                                startActivityForResult(
                                                                        new Intent(
                                                                                Intent.ACTION_OPEN_DOCUMENT)
                                                                                .setType("image/*")
                                                                                .addCategory(
                                                                                        Intent.CATEGORY_OPENABLE),
                                                                        11);
                                                            else
                                                                saveIcon(null);
                                                        })
                                                .setNegativeButton(android.R.string.cancel, null)
                                                .show());
                        continue;
                    }
                    if (row.length == 3 && row[2].equals("readonly")) {
                        TextView value = Ui.text(
                                this, fieldName(row[0]) + ": " + row[1], 16, Ui.TEXT, false);
                        value.setTextIsSelectable(true);
                        entries.addView(value, Ui.space(this, -2, 12));
                        continue;
                    }
                    String display = row[1];
                    for (int i = 2; i < row.length; i++)
                        if (row[i].startsWith(row[1] + ":"))
                            display = row[i].substring(row[i].indexOf(':') + 1);
                    button(fieldName(row[0]) + ": "
                                    + ProfileLabels.value(this, row[0], row[1], display),
                            () -> {
                                if (row.length > 2) {
                                    String[] labels = new String[row.length - 2];
                                    for (int i = 2; i < row.length; i++) {
                                        int colon = row[i].indexOf(':');
                                        labels[i - 2] = ProfileLabels.value(this, row[0],
                                                row[i].substring(0, colon),
                                                row[i].substring(colon + 1));
                                    }
                                    new AlertDialog.Builder(this)
                                            .setTitle(fieldName(row[0]))
                                            .setItems(labels,
                                                    (d, index) -> {
                                                        String selected = row[index + 2];
                                                        change("profile", row[0],
                                                                selected.substring(
                                                                        0, selected.indexOf(':')),
                                                                null);
                                                    })
                                            .setNegativeButton(android.R.string.cancel, null)
                                            .show();
                                } else
                                    edit("profile", row[0], row[1], fieldName(row[0]));
                            });
                } else if (page.equals("patches")) {
                    Switch toggle = new Switch(this);
                    toggle.setText(row[1]);
                    toggle.setTextColor(Ui.TEXT);
                    toggle.setChecked(Boolean.parseBoolean(row[4]));
                    toggle.setOnCheckedChangeListener((b, on) -> {
                        if (!busy)
                            change("patch", row[0], row[3] + ":" + on, null);
                    });
                    entries.addView(toggle, Ui.space(this, -2, 4));
                    entries.addView(
                            Ui.text(this, row[2], 13, Ui.MUTED, false), Ui.space(this, -2, 12));
                } else if (page.equals("stats")) {
                    if (row[0].equals("compatibility")) {
                        String[] states = getResources().getStringArray(R.array.compat_states);
                        entries.addView(Ui.text(this,
                                getString(R.string.desktop_compatibility) + ": "
                                        + states[Integer.parseInt(row[1])],
                                16, Ui.TEXT, true));
                        for (int i = 2; i < 4; ++i) {
                            final String url = row[i];
                            if (url.startsWith("https://github.com/"))
                                button(i == 2 ? "Canary" : "Master",
                                        ()
                                                -> startActivity(new Intent(
                                                        Intent.ACTION_VIEW, Uri.parse(url))));
                        }
                    } else
                        entries.addView(Ui.text(this,
                                                row[1] + "\n" + getString(R.string.achievements)
                                                        + ": " + row[2] + " · " + row[3],
                                                16, Ui.TEXT, false),
                                Ui.space(this, -2, 12));
                } else {
                    button(row[1] + "\n" + row[2]
                                    + (row.length > 5 ? " · "
                                                            + android.text.format.Formatter
                                                                    .formatFileSize(this,
                                                                            Long.parseLong(row[5]))
                                                      : ""),
                            () -> {
                                AlertDialog.Builder dialog =
                                        new AlertDialog.Builder(this)
                                                .setTitle(row[1])
                                                .setMessage(row[3])
                                                .setNegativeButton(android.R.string.cancel, null);
                                if (row[4].equals("link"))
                                    dialog.setPositiveButton(R.string.detach_content,
                                            (d, w) -> change("content_remove", row[3], "", null));
                                dialog.show();
                            });
                }
            }
        });
    }
    private void button(String text, Runnable action) {
        Button b = Ui.button(this, text, false);
        b.setOnClickListener(v -> {
            if (!busy)
                action.run();
        });
        entries.addView(b, Ui.space(this, -2, 8));
    }
    private void profileMenu(String[] row) {
        xuid = Long.parseUnsignedLong(row[0], 16);
        new AlertDialog.Builder(this)
                .setTitle(row[1])
                .setItems(new String[] {getString(R.string.sign_in), getString(R.string.sign_out),
                                  getString(R.string.edit_profile)},
                        (d, which) -> {
                            if (which == 0) {
                                String[] players = new String[4];
                                for (int i = 0; i < 4; i++)
                                    players[i] = getString(R.string.player_number, i + 1);
                                new AlertDialog.Builder(this)
                                        .setItems(players,
                                                (p, slot)
                                                        -> change("login", "",
                                                                Integer.toString(slot), null))
                                        .show();
                            } else if (which == 1)
                                change("logout", "", "", null);
                            else {
                                page = "profile";
                                refresh();
                            }
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
    private String fieldName(String field) {
        switch (field) {
            case "gamertag":
                return getString(R.string.profile_gamertag);
            case "country":
                return getString(R.string.profile_country);
            case "language":
                return getString(R.string.profile_language);
            case "live":
                return getString(R.string.profile_live);
            case "online_xuid":
                return getString(R.string.profile_online_xuid);
            case "online_domain":
                return getString(R.string.profile_online_domain);
            case "gamer_name":
                return getString(R.string.gamer_name);
            case "motto":
                return getString(R.string.gamer_motto);
            case "bio":
                return getString(R.string.gamer_bio);
            case "zone":
                return getString(R.string.gamer_zone);
            default:
                return getString(R.string.profile_subscription);
        }
    }
    private void edit(String operation, String key, String initial, String label) {
        EditText text = new EditText(this);
        text.setSingleLine(!key.equals("bio"));
        text.setText(initial);
        AlertDialog dialog = new AlertDialog.Builder(this)
                                     .setTitle(label)
                                     .setView(text)
                                     .setPositiveButton(R.string.options_save, null)
                                     .setNegativeButton(android.R.string.cancel, null)
                                     .create();
        dialog.setOnShowListener(d
                -> dialog.getButton(-1).setOnClickListener(
                        v -> change(operation, key, text.getText().toString(), dialog)));
        dialog.show();
    }
    private void change(String operation, String key, String value, AlertDialog dialog) {
        if (busy)
            return;
        busy = true;
        status.setText(R.string.working);
        for (int i = 0; i < entries.getChildCount(); i++) entries.getChildAt(i).setEnabled(false);
        if (dialog != null) {
            dialog.getButton(-1).setEnabled(false);
            dialog.setCancelable(false);
        }
        worker.execute(() -> {
            try {
                AndroidTools.changeNative(
                        getNativeAppContext(), operation, title, xuid, key, value);
                if (dialog != null)
                    runOnUiThread(dialog::dismiss);
                load();
            } catch (Exception e) {
                failed(e);
                if (dialog != null)
                    runOnUiThread(() -> {
                        if (isDestroyed())
                            return;
                        dialog.getButton(-1).setEnabled(true);
                        dialog.setCancelable(true);
                        new AlertDialog.Builder(this)
                                .setMessage(getString(R.string.operation_error, e.getMessage()))
                                .setPositiveButton(android.R.string.ok, null)
                                .show();
                    });
            }
        });
    }
    private void refresh() {
        busy = true;
        worker.execute(() -> {
            try {
                load();
            } catch (Exception e) {
                failed(e);
            }
        });
    }
    private void failed(Exception e) {
        runOnUiThread(() -> {
            busy = false;
            if (!isDestroyed()) {
                status.setText(getString(R.string.operation_error, e.getMessage()));
                for (int i = 0; i < entries.getChildCount(); i++)
                    entries.getChildAt(i).setEnabled(true);
            }
        });
    }
    private void pickContent() {
        if (!android.os.Environment.isExternalStorageManager()) {
            new AlertDialog.Builder(this)
                    .setMessage(R.string.storage_explanation)
                    .setPositiveButton(R.string.open_settings,
                            (d, w)
                                    -> startActivity(new Intent(
                                            android.provider.Settings
                                                    .ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                            Uri.parse("package:" + getPackageName()))))
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
            return;
        }
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                                       .setType("*/*")
                                       .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                                       .addCategory(Intent.CATEGORY_OPENABLE)
                                       .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION),
                10);
    }
    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == 11 && result == RESULT_OK && data != null && data.getData() != null) {
            saveIcon(data.getData());
            return;
        }
        if (request != 10 || result != RESULT_OK || data == null)
            return;
        java.util.ArrayList<Uri> uris = new java.util.ArrayList<>();
        if (data.getClipData() != null) {
            for (int i = 0; i < data.getClipData().getItemCount(); i++)
                uris.add(data.getClipData().getItemAt(i).getUri());
        } else if (data.getData() != null)
            uris.add(data.getData());
        importPackages(uris, false);
    }
    private void importPackages(java.util.List<Uri> uris, boolean extract) {
        if (busy || uris.isEmpty())
            return;
        busy = true;
        status.setText(R.string.working);
        for (int i = 0; i < entries.getChildCount(); i++) entries.getChildAt(i).setEnabled(false);
        final long session = AndroidTools.beginInstallNative();
        java.util.concurrent.atomic.AtomicBoolean cancelled = new java.util.concurrent.atomic.AtomicBoolean();
        LinearLayout progressView = Ui.column(this);
        progressView.setPadding(Ui.dp(this, 24), Ui.dp(this, 12), Ui.dp(this, 24), 0);
        TextView current = Ui.text(this, getString(R.string.working), 14, Ui.TEXT, false);
        TextView bytes = Ui.text(this, "", 13, Ui.MUTED, false);
        ProgressBar progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progressView.addView(current);
        progressView.addView(progress, Ui.space(this, 24, 8));
        progressView.addView(bytes);
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(R.string.install_content)
                .setView(progressView).setCancelable(false)
                .setNegativeButton(android.R.string.cancel, null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_NEGATIVE)
                .setOnClickListener(v -> {
                    cancelled.set(true);
                    AndroidTools.installProgressNative(session, true);
                    dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(false);
                }));
        dialog.show();
        android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
        Runnable poll = new Runnable() {
            @Override public void run() {
                if (!dialog.isShowing() || isDestroyed()) return;
                long[] value = AndroidTools.installProgressNative(session, false);
                progress.setIndeterminate(value[1] <= 0);
                if (value[1] > 0) {
                    progress.setProgress((int) Math.min(1000, (double) value[0] / value[1] * 1000));
                    bytes.setText(android.text.format.Formatter.formatFileSize(ToolsActivity.this, value[0])
                            + " / " + android.text.format.Formatter.formatFileSize(ToolsActivity.this, value[1]));
                } else bytes.setText("");
                handler.postDelayed(this, 200);
            }
        };
        handler.post(poll);
        worker.execute(() -> {
            java.util.ArrayList<Uri> mutable = new java.util.ArrayList<>();
            StringBuilder results = new StringBuilder();
            int index = 0;
            for (Uri uri : uris) {
                if (isFinishing() || cancelled.get())
                    break;
                ++index;
                String name = uri.getLastPathSegment();
                try {
                    File source = AndroidStorage.file(this, uri);
                    name = source.getName();
                    final String label = index + " / " + uris.size() + " · " + name;
                    runOnUiThread(() -> {
                        current.setText(label);
                        progress.setIndeterminate(true);
                    });
                    if (cancelled.get()) break;
                    if (page.equals("install")) {
                        if (!AndroidTools.installPackageNative(
                                    getNativeAppContext(), source.getAbsolutePath(), extract, session)) {
                            mutable.add(uri);
                            continue;
                        }
                    } else
                        AndroidTools.changeNative(getNativeAppContext(), "content_add", title, 0,
                                Integer.toUnsignedString(version), source.getAbsolutePath());
                    results.append("✓ ").append(name).append('\n');
                } catch (Exception e) {
                    results.append(name).append(": ").append(cancelled.get()
                            ? getString(R.string.install_cancelled) : e.getMessage()).append('\n');
                }
            }
            if (cancelled.get()) results.append(getString(R.string.install_cancelled)).append('\n');
            AndroidTools.endInstallNative(session);
            runOnUiThread(() -> {
                handler.removeCallbacks(poll);
                dialog.dismiss();
                busy = false;
                if (isDestroyed() || isFinishing())
                    return;
                if (!mutable.isEmpty() && !cancelled.get()) {
                    new AlertDialog.Builder(this)
                            .setTitle(R.string.mutable_content)
                            .setMessage(getString(R.string.mutable_content_explanation) + "\n\n"
                                    + results)
                            .setPositiveButton(
                                    R.string.extract_data, (d, w) -> importPackages(mutable, true))
                            .setNegativeButton(android.R.string.cancel, (d, w) -> refresh())
                            .setOnCancelListener(d -> refresh())
                            .show();
                } else {
                    refresh();
                    new AlertDialog.Builder(this)
                            .setTitle(R.string.install_content)
                            .setMessage(results)
                            .setPositiveButton(android.R.string.ok, null)
                            .show();
                }
            });
        });
    }

    private void saveIcon(Uri uri) {
        if (busy)
            return;
        busy = true;
        status.setText(R.string.working);
        worker.execute(() -> {
            try {
                byte[] png = null;
                if (uri != null) {
                    android.graphics.ImageDecoder.Source source =
                            android.graphics.ImageDecoder.createSource(getContentResolver(), uri);
                    android.graphics.Bitmap bitmap = android.graphics.ImageDecoder.decodeBitmap(
                            source, (decoder, info, src) -> {
                                decoder.setTargetSize(64, 64);
                                decoder.setAllocator(
                                        android.graphics.ImageDecoder.ALLOCATOR_SOFTWARE);
                            });
                    try (java.io.ByteArrayOutputStream output =
                                    new java.io.ByteArrayOutputStream()) {
                        if (!bitmap.compress(
                                    android.graphics.Bitmap.CompressFormat.PNG, 100, output))
                            throw new java.io.IOException(getString(R.string.invalid_image));
                        png = output.toByteArray();
                    } finally {
                        bitmap.recycle();
                    }
                }
                AndroidTools.iconNative(getNativeAppContext(), xuid, png);
                load();
            } catch (Exception e) {
                failed(e);
            }
        });
    }
    private int label(String name) {
        switch (name) {
            case "install":
                return R.string.install_content;
            case "profiles":
                return R.string.profiles;
            case "profile":
                return R.string.edit_profile;
            case "patches":
                return R.string.patches;
            case "content":
                return R.string.content;
            case "saves":
                return R.string.saves;
            default:
                return R.string.statistics;
        }
    }
    @Override
    public void onBackPressed() {
        if (busy) {
            moveTaskToBack(true);
            return;
        }
        if (page.equals("profile")) {
            page = "profiles";
            refresh();
        } else
            finish();
    }
    @Override
    protected void onSaveInstanceState(Bundle out) {
        out.putString("page", page);
        out.putLong("xuid", xuid);
        super.onSaveInstanceState(out);
    }
    @android.annotation.SuppressLint("MissingSuperCall")
    @Override
    protected void onDestroy() {
        // Match the player process: don't free a core beneath a pending IO operation.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
