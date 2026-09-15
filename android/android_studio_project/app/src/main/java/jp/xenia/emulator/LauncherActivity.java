package jp.xenia.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.*;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public class LauncherActivity extends LocalizedActivity {
    private static final int OPEN_IMAGE = 10, IMPORT_TREE = 11, RELOCATE = 14;
    private GameLibrary library;
    private GameIcons icons;
    private SharedPreferences settings;
    private LinearLayout root, page, gameList;
    private TextView importStatus;
    private String query = "", progressText = "";
    private boolean showingSettings;
    private boolean importing, indexing;
    private Runnable afterStorageAccess;
    private String relocateId;
    private boolean checkingLaunch;
    private final AtomicBoolean cancelImport = new AtomicBoolean();
    private final ExecutorService io = Executors.newSingleThreadExecutor();

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        library = new GameLibrary(this);
        icons = new GameIcons(this);
        settings = getSharedPreferences("android_settings", MODE_PRIVATE);
        if (state != null) {
            showingSettings = state.getBoolean("showing_settings");
            query = state.getString("query", "");
            relocateId = state.getString("relocate_id");
        }
        buildScreen();
        indexMetadata();
    }
    @Override
    protected void onResume() {
        super.onResume();
        refreshHistory(4);
        // Disc selection runs in :emulation and registers through the provider.
        GameLibrary updated = new GameLibrary(this);
        if (updated.games.size() != library.games.size()) {
            library = updated;
            buildScreen();
        }
        indexMetadata();
    }
    @Override
    public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        buildScreen();
    }
    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putBoolean("showing_settings", showingSettings);
        out.putString("query", query);
        out.putString("relocate_id", relocateId);
    }
    private void buildScreen() {
        getWindow().setStatusBarColor(Ui.BG);
        getWindow().setNavigationBarColor(Ui.BG);
        root = Ui.column(this);
        root.setBackgroundColor(Ui.BG);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
            v.setPadding(Ui.dp(this, 20) + bars.left, Ui.dp(this, 12) + bars.top,
                    Ui.dp(this, 20) + bars.right, bars.bottom);
            return insets;
        });
        setContentView(root);
        LinearLayout brand = Ui.row(this);
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.xenia_edge_icon);
        logo.setScaleType(ImageView.ScaleType.FIT_CENTER);
        logo.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        brand.addView(logo, new LinearLayout.LayoutParams(Ui.dp(this, 44), Ui.dp(this, 44)));
        LinearLayout wordmark = Ui.row(this);
        wordmark.setLayoutDirection(View.LAYOUT_DIRECTION_LTR);
        wordmark.addView(Ui.text(this, "  XENIA  ", 22, Ui.TEXT, true));
        wordmark.addView(Ui.text(this, "EDGE", 11, Ui.GREEN, true));
        brand.addView(wordmark);
        brand.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1));
        ImageButton preferences = new ImageButton(this);
        preferences.setId(R.id.library_settings);
        preferences.setImageResource(R.drawable.ic_settings);
        preferences.setContentDescription(getString(R.string.settings));
        preferences.setTooltipText(getString(R.string.settings));
        preferences.setPadding(Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12));
        preferences.setBackground(new android.graphics.drawable.RippleDrawable(
                android.content.res.ColorStateList.valueOf(0x3344BB44), Ui.shape(this, Ui.CARD, 10),
                null));
        preferences.setOnClickListener(v -> {
            showingSettings = true;
            buildScreen();
        });
        brand.addView(preferences, new LinearLayout.LayoutParams(Ui.dp(this, 48), Ui.dp(this, 48)));
        root.addView(brand, Ui.space(this, 52, 8));
        page = Ui.column(this);
        root.addView(page, new LinearLayout.LayoutParams(-1, 0, 1));
        if (showingSettings)
            buildSettings();
        else
            buildLibrary();
    }
    private void buildLibrary() {
        LinearLayout heading = Ui.row(this);
        TextView title = Ui.text(this, getString(R.string.your_games), 24, Ui.TEXT, true);
        heading.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        Button add = Ui.button(this, getString(R.string.add_games), true);
        add.setId(R.id.library_add_games);
        add.setSingleLine(true);
        add.setTextSize(13);
        add.setPadding(Ui.dp(this, 12), 0, Ui.dp(this, 12), 0);
        LinearLayout.LayoutParams addSize = new LinearLayout.LayoutParams(-2, Ui.dp(this, 48));
        addSize.setMarginStart(Ui.dp(this, 8));
        heading.addView(add, addSize);
        add.setOnClickListener(v -> showAddMenu(add));
        page.addView(heading, Ui.space(this, -2, 10));
        if (importing) {
            importStatus = Ui.text(this, progressText, 12, Ui.GREEN, false);
            page.addView(importStatus, Ui.space(this, -2, 4));
            ProgressBar bar =
                    new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
            bar.setIndeterminate(true);
            page.addView(bar, Ui.space(this, 4, 4));
            Button cancel = Ui.button(this, getString(android.R.string.cancel), false);
            cancel.setOnClickListener(v -> cancelImport.set(true));
            page.addView(cancel, Ui.space(this, 40, 8));
        } else
            importStatus = null;
        EditText search = new EditText(this);
        search.setSingleLine(true);
        search.setTextSize(14);
        search.setTextColor(Ui.TEXT);
        search.setHintTextColor(Ui.MUTED);
        search.setHint(R.string.search_games);
        search.setBackground(Ui.shape(this, Ui.CARD, 12));
        search.setPadding(Ui.dp(this, 14), 0, Ui.dp(this, 14), 0);
        search.setText(query);
        LinearLayout searchRow = Ui.row(this);
        searchRow.addView(search, new LinearLayout.LayoutParams(0, Ui.dp(this, 48), 1));
        Button sort = Ui.button(this, "↕", false);
        sort.setContentDescription(getString(R.string.sort_games));
        sort.setOnClickListener(v
                -> new AlertDialog.Builder(this)
                        .setTitle(R.string.sort_games)
                        .setItems(getResources().getStringArray(R.array.game_sorts),
                                (d, which) -> {
                                    settings.edit().putInt("library_sort", which).apply();
                                    renderGames();
                                })
                        .setNegativeButton(android.R.string.cancel, null)
                        .show());
        searchRow.addView(sort, new LinearLayout.LayoutParams(Ui.dp(this, 48), Ui.dp(this, 48)));
        page.addView(searchRow, Ui.space(this, 48, 14));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        gameList = Ui.column(this);
        scroll.addView(gameList);
        page.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        search.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int c, int f) {}
            public void onTextChanged(CharSequence s, int a, int before, int count) {
                query = s.toString();
                renderGames();
            }
            public void afterTextChanged(Editable e) {}
        });
        renderGames();
    }
    private void showAddMenu(View anchor) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add(0, OPEN_IMAGE, 0, R.string.single_game);
        menu.getMenu().add(0, IMPORT_TREE, 1, R.string.game_folder).setEnabled(!importing);
        menu.setOnMenuItemClickListener(item -> {
            withStorageAccess(() -> {
                if (item.getItemId() == OPEN_IMAGE) {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*");
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(intent, OPEN_IMAGE);
                } else {
                    new AlertDialog.Builder(this)
                            .setTitle(R.string.game_folder)
                            .setMessage(R.string.import_explanation)
                            .setNegativeButton(android.R.string.cancel, null)
                            .setPositiveButton(R.string.choose_folder,
                                    (d, w)
                                            -> startActivityForResult(
                                                    new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                                                            .addFlags(
                                                                    Intent.FLAG_GRANT_READ_URI_PERMISSION),
                                                    IMPORT_TREE))
                            .show();
                }
            });
            return true;
        });
        menu.show();
    }
    private void renderGames() {
        gameList.removeAllViews();
        List<GameLibrary.Game> shown = new ArrayList<>();
        java.util.Map<String, List<GameLibrary.Game>> groups = library.groups();
        java.util.Map<String, Long> played = new java.util.HashMap<>();
        for (java.util.Map.Entry<String, List<GameLibrary.Game>> entry : groups.entrySet())
            played.put(entry.getKey(),
                    entry.getValue().stream().mapToLong(g -> g.played).max().orElse(0));
        for (GameLibrary.Game g : library.releases())
            if (g.name.toLowerCase(Locale.ROOT).contains(query.toLowerCase(Locale.ROOT))
                    || groups.get(g.releaseKey())
                            .stream()
                            .anyMatch(disc
                                    -> disc.path.toLowerCase(Locale.ROOT)
                                            .contains(query.toLowerCase(Locale.ROOT))))
                shown.add(g);
        int sort = settings.getInt("library_sort", 0);
        Comparator<GameLibrary.Game> order = sort / 2 == 1
                ? Comparator.comparingLong(g -> played.get(g.releaseKey()))
                : sort / 2 == 2 ? Comparator.comparingInt(g -> g.compatibility)
                                : Comparator.comparing(g -> g.name.toLowerCase(Locale.ROOT));
        if (sort % 2 != 0)
            order = order.reversed();
        shown.sort(order.thenComparing(g -> g.name.toLowerCase(Locale.ROOT))
                        .thenComparing(g -> g.path));
        if (shown.isEmpty()) {
            LinearLayout empty = Ui.column(this);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(20, Ui.dp(this, 48), 20, 20);
            TextView plus = Ui.text(this, "＋", 48, Ui.GREEN, false);
            plus.setGravity(Gravity.CENTER);
            empty.addView(plus);
            TextView t = Ui.text(this,
                    getString(query.isEmpty() ? R.string.empty_library : R.string.no_results), 19,
                    Ui.TEXT, true);
            t.setGravity(Gravity.CENTER);
            empty.addView(t, Ui.space(this, -2, 12));
            TextView hint = Ui.text(this, getString(R.string.empty_hint), 14, Ui.MUTED, false);
            hint.setGravity(Gravity.CENTER);
            empty.addView(hint);
            gameList.addView(empty);
            return;
        }
        gameList.addView(Ui.text(this,
                                 getResources().getQuantityString(
                                         R.plurals.game_count, shown.size(), shown.size()),
                                 12, Ui.MUTED, false),
                Ui.space(this, -2, 10));
        int columns = getResources().getConfiguration().screenWidthDp >= 600 ? 3 : 2;
        for (int i = 0; i < shown.size(); i += columns) {
            LinearLayout row = Ui.row(this);
            row.setGravity(Gravity.TOP);
            for (int j = 0; j < columns; ++j) {
                LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, -2, 1);
                p.setMarginStart(j == 0 ? 0 : Ui.dp(this, 5));
                p.setMarginEnd(j == columns - 1 ? 0 : Ui.dp(this, 5));
                p.bottomMargin = Ui.dp(this, 10);
                if (i + j >= shown.size()) {
                    row.addView(new View(this), p);
                    continue;
                }
                GameLibrary.Game g = shown.get(i + j);
                LinearLayout card = Ui.column(this);
                card.setPadding(Ui.dp(this, 14), Ui.dp(this, 16), Ui.dp(this, 14), Ui.dp(this, 14));
                card.setBackground(Ui.shape(this, Ui.CARD, 16));
                card.setContentDescription(getString(R.string.play_named, g.name));
                card.addView(new GameArtwork(this, icons, g), Ui.space(this, -2, 12));
                TextView name = Ui.text(this, g.name, 16, Ui.TEXT, true);
                name.setMinLines(2);
                name.setMaxLines(2);
                name.setEllipsize(android.text.TextUtils.TruncateAt.END);
                card.addView(name, Ui.space(this, -2, 8));
                card.setOnClickListener(v -> play(g));
                card.setOnLongClickListener(v -> {
                    details(g);
                    return true;
                });
                row.addView(card, p);
            }
            gameList.addView(row);
        }
    }
    private void play(GameLibrary.Game game) {
        withStorageAccess(() -> {
            if (checkingLaunch) return;
            checkingLaunch = true;
            io.execute(() -> {
                boolean available = false;
                try {
                    java.io.File file = game.path.startsWith("content://")
                            ? AndroidStorage.file(this, Uri.parse(game.path))
                            : new java.io.File(game.path);
                    available = file.isFile() && file.canRead();
                } catch (java.io.IOException ignored) {
                }
                final boolean readable = available;
                runOnUiThread(() -> {
                    checkingLaunch = false;
                    if (isFinishing() || isDestroyed()) return;
                    if (readable) launchGame(game);
                    else new AlertDialog.Builder(this)
                            .setMessage(getString(R.string.file_unavailable) + "\n\n" + game.path)
                            .setPositiveButton(R.string.locate_game, (d, w) -> {
                                relocateId = game.id;
                                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                                        .setType("*/*").addCategory(Intent.CATEGORY_OPENABLE), RELOCATE);
                            })
                            .setNegativeButton(android.R.string.cancel, null).show();
                });
            });
        });
    }
    private void launchGame(GameLibrary.Game game) {
        game.played = System.currentTimeMillis();
        library.save();
        startActivity(new Intent(this, EmulatorActivity.class)
                        .putExtra("game_name", game.name)
                        .putExtra("game_path", game.path)
                        .putExtra("resolution", settings.getInt("resolution", -1))
                        .putExtra("fps_limit", settings.getInt("fps_limit", -1))
                        .putExtra("touch_controls", settings.getBoolean("touch_controls", true)));
    }
    private void details(GameLibrary.Game game) {
        new AlertDialog.Builder(this)
                .setTitle(game.name)
                .setItems(
                        new String[] {getString(R.string.game_information),
                                getString(R.string.game_options), getString(R.string.play),
                                getString(R.string.remove_library), getString(R.string.rename_game),
                                getString(R.string.manage_discs), getString(R.string.open_folder)},
                        (d, which) -> {
                            if (which == 0) {
                                startActivity(new Intent(this, GameInfoActivity.class)
                                                .putExtra("game_id", game.id));
                            } else if (which == 1) {
                                startActivity(game.identify(new Intent(this, GameSettingsActivity.class)));
                            } else if (which == 2) {
                                play(game);
                            } else if (which == 4) {
                                EditText name = new EditText(this);
                                name.setText(game.name);
                                name.setSingleLine();
                                AlertDialog rename =
                                        new AlertDialog.Builder(this)
                                                .setTitle(R.string.rename_game)
                                                .setView(name)
                                                .setPositiveButton(R.string.options_save, null)
                                                .setNegativeButton(android.R.string.cancel, null)
                                                .create();
                                rename.setOnShowListener(v
                                        -> rename.getButton(AlertDialog.BUTTON_POSITIVE)
                                                .setOnClickListener(button -> {
                                                    String value = name.getText().toString().trim();
                                                    if (value.isEmpty()) {
                                                        name.setError(
                                                                getString(R.string.invalid_name));
                                                        return;
                                                    }
                                                    for (GameLibrary.Game disc :
                                                            library.group(game))
                                                        disc.name = value;
                                                    library.save();
                                                    renderGames();
                                                    rename.dismiss();
                                                }));
                                rename.show();
                            } else if (which == 6) {
                                try {
                                    java.io.File file = game.path.startsWith("content://")
                                            ? AndroidStorage.file(this, Uri.parse(game.path))
                                            : new java.io.File(game.path);
                                    AndroidStorage.openDirectory(this, file.getParentFile());
                                } catch (Exception e) {
                                    Toast.makeText(this,
                                                 getString(
                                                         R.string.operation_error, e.getMessage()),
                                                 Toast.LENGTH_LONG)
                                            .show();
                                }
                            } else if (which == 5) {
                                manageDiscs(game);
                            } else {
                                library.games.removeAll(library.group(game));
                                library.save();
                                renderGames();
                            }
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
    private void manageDiscs(GameLibrary.Game game) {
        List<GameLibrary.Game> discs = library.group(game);
        GameLibrary.Game effective = library.releases().stream()
                .filter(g -> g.releaseKey().equals(game.releaseKey())).findFirst().orElse(game);
        String[] names = new String[discs.size()];
        for (int i = 0; i < discs.size(); i++) {
            GameLibrary.Game disc = discs.get(i);
            names[i] = (disc == effective ? "✓ " : "")
                    + getString(R.string.disc_label, disc.disc > 0 ? disc.disc : i + 1)
                    + (disc.mediaId == 0 ? "" : " · " + String.format(Locale.ROOT, "%08X", disc.mediaId))
                    + " · " + disc.path;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.manage_discs)
                .setItems(names,
                        (d, selected) -> {
                            GameLibrary.Game disc = discs.get(selected);
                            new AlertDialog.Builder(this)
                                    .setTitle(names[selected])
                                    .setItems(new String[] {getString(R.string.play),
                                                      getString(R.string.default_disc),
                                                      getString(R.string.remove_library)},
                                            (choice, which) -> {
                                                if (which == 0) {
                                                    play(disc);
                                                    return;
                                                }
                                                if (which == 1)
                                                    for (GameLibrary.Game item : discs)
                                                        item.preferred = item == disc;
                                                else
                                                    library.games.remove(disc);
                                                library.save();
                                                renderGames();
                                            })
                                    .setNegativeButton(android.R.string.cancel, null)
                                    .show();
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
    private void indexMetadata() {
        if (indexing)
            return;
        indexing = true;
        List<GameLibrary.Game> snapshot = new ArrayList<>(library.games);
        io.execute(() -> {
            java.util.Map<String, long[]> values = new java.util.HashMap<>();
            for (GameLibrary.Game game : snapshot) {
                if (cancelImport.get() || isDestroyed())
                    break;
                try {
                    String path = game.path;
                    java.io.File file = path.startsWith("content://")
                            ? AndroidStorage.file(this, Uri.parse(path))
                            : new java.io.File(path);
                    long stamp = GameLibrary.metadataStamp(file);
                    if (file.isFile() && (game.metadataStamp != stamp || !game.metadataCached)) {
                        int[] data = GameIcons.metadataNative(file.getAbsolutePath());
                        if (data != null && data.length >= 6)
                            values.put(game.id + "\n" + path,
                                    new long[] {data[0], data[1], data[2], data[3], data[4], data[5],
                                            GameIcons.compatibilityNative(data[0]), stamp});
                    }
                } catch (java.io.IOException ignored) {
                }
            }
            runOnUiThread(() -> {
                indexing = false;
                if (isDestroyed())
                    return;
                for (GameLibrary.Game game : library.games) {
                    long[] data = values.get(game.id + "\n" + game.path);
                    if (data == null)
                        continue;
                    game.metadata(new int[] {(int) data[0], (int) data[1], (int) data[2],
                            (int) data[3], (int) data[4], (int) data[5]}, data[7]);
                    game.compatibility = (int) data[6];
                }
                if (!values.isEmpty()) {
                    library.save();
                    if (!showingSettings)
                        renderGames();
                }
                if (library.games.stream().anyMatch(
                            g -> snapshot.stream().noneMatch(old -> old.id.equals(g.id))))
                    indexMetadata();
            });
        });
    }
    private void relocate(String id, Uri uri) {
        GameLibrary.Game game = library.games.stream().filter(g -> g.id.equals(id))
                .findFirst().orElse(null);
        if (game == null) return;
        String oldPath = game.path;
        int title = game.titleId, version = game.version;
        io.execute(() -> {
            try {
                java.io.File file = AndroidStorage.file(this, uri);
                String format = GameLibrary.format(file);
                int[] info = GameIcons.readMetadata(file.getAbsolutePath());
                if (format == null)
                    throw new java.io.IOException(getString(R.string.unsupported_game));
                if (title != 0 && (info == null || info.length < 6
                        || info[0] != title || (version != 0 && info[2] != version)))
                    throw new java.io.IOException(getString(R.string.wrong_game_release));
                getContentResolver().takePersistableUriPermission(uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION);
                long stamp = GameLibrary.metadataStamp(file);
                runOnUiThread(() -> {
                    if (isDestroyed() || isFinishing() || !library.games.contains(game)
                            || !game.path.equals(oldPath)) return;
                    library.replace(game, file.getAbsolutePath(), format, info, stamp);
                    buildScreen();
                    play(game);
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    if (!isDestroyed() && !isFinishing()) error(e.getMessage());
                });
            }
        });
    }
    private void refreshHistory(int retries) {
        io.execute(() -> {
            try (DataLock lock = new DataLock(getFilesDir())) {
                android.util.AtomicFile config = new android.util.AtomicFile(
                        new java.io.File(getFilesDir(), "xenia-edge.config.toml"));
                String document = config.getBaseFile().exists()
                        ? new String(config.readFully(), java.nio.charset.StandardCharsets.UTF_8) : "";
                long[] rows = GameSettings.historyNative(
                        AndroidStorage.root(this, "content").getAbsolutePath(), document);
                runOnUiThread(() -> {
                    if (isDestroyed() || isFinishing()) return;
                    library.history(rows);
                    if (!showingSettings) renderGames();
                });
            } catch (java.io.IOException | IllegalArgumentException e) {
                // A closing management process may briefly still own the lock.
                if (retries > 0) runOnUiThread(() -> root.postDelayed(() -> {
                    if (!isDestroyed() && !isFinishing()) refreshHistory(retries - 1);
                }, 250));
                android.util.Log.d("xenia", "Profile history unavailable", e);
            }
        });
    }
    private void buildSettings() {
        Button back = Ui.button(this, "‹  " + getString(R.string.settings), false);
        back.setContentDescription(getString(R.string.back_library));
        back.setOnClickListener(v -> {
            showingSettings = false;
            buildScreen();
        });
        page.addView(back, Ui.space(this, 48, 12));
        ScrollView scroll = new ScrollView(this);
        LinearLayout content = Ui.column(this);
        scroll.addView(content);
        page.addView(scroll);
        content.addView(Ui.text(this, getString(R.string.video), 12, Ui.GREEN, true),
                Ui.space(this, -2, 12));
        Button options = Ui.button(this, getString(R.string.global_options), false);
        options.setOnClickListener(
                v -> startActivity(new Intent(this, GameSettingsActivity.class)));
        content.addView(options, Ui.space(this, 48, 12));
        Button language = Ui.button(this, getString(R.string.interface_language), false);
        language.setOnClickListener(v -> AppLanguage.choose(this));
        content.addView(language, Ui.space(this, 48, 12));
        Button profiles = Ui.button(this, getString(R.string.profiles), false);
        profiles.setOnClickListener(v
                -> startActivity(
                        new Intent(this, ToolsActivity.class).putExtra("page", "profiles")));
        content.addView(profiles, Ui.space(this, 48, 12));
        Button files = Ui.button(this, getString(R.string.manage_files), false);
        files.setOnClickListener(v -> XeniaDocumentsProvider.open(this, "root"));
        content.addView(files, Ui.space(this, 48, 12));
        Button plugins = Ui.button(this, getString(R.string.plugins), false);
        plugins.setOnClickListener(v -> XeniaDocumentsProvider.open(this, "plugins"));
        content.addView(plugins, Ui.space(this, 48, 12));
        Button install = Ui.button(this, getString(R.string.install_content), false);
        install.setOnClickListener(v
                -> startActivity(
                        new Intent(this, ToolsActivity.class).putExtra("page", "install")));
        content.addView(install, Ui.space(this, 48, 12));
        Button trace = Ui.button(this, getString(R.string.open_trace), false);
        trace.setOnClickListener(v
                -> withStorageAccess(()
                                             -> startActivityForResult(
                                                     new Intent(Intent.ACTION_OPEN_DOCUMENT)
                                                             .setType("*/*")
                                                             .addCategory(Intent.CATEGORY_OPENABLE),
                                                     13)));
        content.addView(trace, Ui.space(this, 48, 12));
        content.addView(
                Ui.text(this, "Vulkan · ARM64", 13, Ui.MUTED, false), Ui.space(this, -2, 24));
        content.addView(Ui.text(this, getString(R.string.controls), 12, Ui.GREEN, true),
                Ui.space(this, -2, 12));
        Switch touch = new Switch(this);
        touch.setText(R.string.touch_controls);
        touch.setTextColor(Ui.TEXT);
        touch.setChecked(settings.getBoolean("touch_controls", true));
        touch.setOnCheckedChangeListener(
                (b, checked) -> settings.edit().putBoolean("touch_controls", checked).apply());
        content.addView(touch, Ui.space(this, 52, 12));
        content.addView(Ui.text(this, getString(R.string.controller_hint), 14, Ui.MUTED, false),
                Ui.space(this, -2, 24));
        content.addView(Ui.text(this, getString(R.string.preview_title), 18, Ui.TEXT, true),
                Ui.space(this, -2, 8));
        content.addView(Ui.text(this, getString(R.string.preview_description), 14, Ui.MUTED, false),
                Ui.space(this, -2, 16));
        Button upstream = Ui.button(this, getString(R.string.project), false);
        upstream.setOnClickListener(v
                -> startActivity(new Intent(
                        Intent.ACTION_VIEW, Uri.parse("https://github.com/has207/xenia-edge"))));
        content.addView(upstream, Ui.space(this, 48, 12));
        int[] help = {R.string.help_faq, R.string.info_compatibility, R.string.about};
        String[] urls = {"https://github.com/xenia-project/xenia/wiki/FAQ",
                "https://github.com/xenia-canary/game-compatibility/issues"};
        for (int i = 0; i < help.length; i++) {
            final int item = i;
            Button button = Ui.button(this, getString(help[i]), false);
            button.setOnClickListener(v -> {
                if (item < 2)
                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(urls[item])));
                else {
                    String[] build = GameSettings.buildNative();
                    new AlertDialog.Builder(this)
                            .setTitle("Xenia Edge")
                            .setMessage(getString(R.string.about_description, versionName())
                                    + "\n\n" + getString(R.string.build_details,
                                            build[0], build[1], build[2]))
                            .setNeutralButton(R.string.build_commit, (d, w) -> startActivity(
                                    new Intent(Intent.ACTION_VIEW, Uri.parse(build[3]))))
                            .setNegativeButton(R.string.license, (d, w) -> startActivity(
                                    new Intent(Intent.ACTION_VIEW, Uri.parse(
                                            "https://github.com/has207/xenia-edge/blob/edge/LICENSE"))))
                            .setPositiveButton(android.R.string.ok, null)
                            .show();
                }
            });
            content.addView(button, Ui.space(this, 48, 12));
        }
    }
    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == RELOCATE) {
            String id = relocateId;
            relocateId = null;
            if (result == RESULT_OK && data != null && data.getData() != null && id != null)
                relocate(id, data.getData());
            return;
        }
        if (request == 13) {
            if (result == RESULT_OK && data != null && data.getData() != null) {
                try {
                    startActivity(new Intent(this, TraceActivity.class)
                                    .putExtra("trace_path",
                                            AndroidStorage.file(this, data.getData())
                                                    .getAbsolutePath()));
                } catch (Exception e) {
                    Toast.makeText(this, getString(R.string.operation_error, e.getMessage()),
                                 Toast.LENGTH_LONG)
                            .show();
                }
            }
            return;
        }
        if (request == 12) {
            Runnable action = afterStorageAccess;
            afterStorageAccess = null;
            if (android.os.Environment.isExternalStorageManager() && action != null)
                action.run();
            else if (!android.os.Environment.isExternalStorageManager())
                error(getString(R.string.storage_explanation));
            return;
        }
        if (result != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        if (request == OPEN_IMAGE) {
            try {
                java.io.File file = AndroidStorage.file(this, uri);
                String name = file.getName(), format = GameLibrary.format(file);
                if (format == null) {
                    error(getString(R.string.unsupported_game));
                    return;
                }
                getContentResolver().takePersistableUriPermission(
                        uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                library.add(new GameLibrary.Game(
                        name.replaceFirst("\\.[^.]+$", ""), file.getAbsolutePath(), format));
                showingSettings = false;
                buildScreen();
                indexMetadata();
            } catch (Exception e) {
                error(e.getMessage());
            }
        } else if (request == IMPORT_TREE && !importing) {
            importing = true;
            cancelImport.set(false);
            progressText = getString(R.string.importing);
            buildScreen();
            io.execute(() -> {
                long[] last = {0};
                try {
                    List<GameLibrary.Game> added =
                            GameLibrary.importTree(this, uri, cancelImport, (name, bytes) -> {
                                long now = android.os.SystemClock.uptimeMillis();
                                if (now - last[0] < 200)
                                    return;
                                last[0] = now;
                                runOnUiThread(() -> {
                                    progressText = getString(
                                            R.string.import_progress, bytes / 1048576, name);
                                    if (importStatus != null)
                                        importStatus.setText(progressText);
                                });
                            });
                    runOnUiThread(() -> {
                        library.addAll(added);
                        indexMetadata();
                        importing = false;
                        showingSettings = false;
                        buildScreen();
                    });
                } catch (Exception e) {
                    runOnUiThread(() -> {
                        importing = false;
                        buildScreen();
                        if (!cancelImport.get())
                            error(e.getMessage());
                    });
                }
            });
        }
    }
    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            return "";
        }
    }
    private void error(String message) {
        if (!isFinishing())
            new AlertDialog.Builder(this)
                    .setTitle(R.string.cannot_open)
                    .setMessage(message)
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
    }
    private void withStorageAccess(Runnable action) {
        if (android.os.Environment.isExternalStorageManager()) {
            action.run();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.storage_access)
                .setMessage(R.string.storage_explanation)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.open_settings,
                        (d, w) -> {
                            afterStorageAccess = action;
                            startActivityForResult(
                                    new Intent(
                                            android.provider.Settings
                                                    .ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                            Uri.parse("package:" + getPackageName())),
                                    12);
                        })
                .show();
    }
    @Override
    public void onBackPressed() {
        if (importing) {
            moveTaskToBack(true);
            return;
        }
        if (showingSettings) {
            showingSettings = false;
            buildScreen();
        } else
            super.onBackPressed();
    }
    @Override
    protected void onDestroy() {
        cancelImport.set(true);
        io.shutdown();
        icons.close();
        super.onDestroy();
    }
}
