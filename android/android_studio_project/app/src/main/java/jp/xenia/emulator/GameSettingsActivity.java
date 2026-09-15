package jp.xenia.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.net.Uri;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.View;
import android.view.WindowInsets;
import android.widget.*;
import java.io.File;
import java.util.*;
import java.util.concurrent.ExecutorService;

/** Searchable per-game options generated from the core's registered cvars. */
public class GameSettingsActivity extends LocalizedActivity {
    private EditText pathTarget;
    private boolean pathDirectory;

    private final ExecutorService worker = GameSettings.IO;
    private GameSettings settings;
    private String[][] options = new String[0][];
    private EditText search;
    private Spinner categories;
    private ListView list;
    private TextView status;
    private boolean saving;
    private boolean global;
    private final List<String[]> visible = new ArrayList<>();

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        global = !getIntent().hasExtra("game_path");
        getWindow().setStatusBarColor(Ui.BG);
        getWindow().setNavigationBarColor(Ui.BG);
        LinearLayout root = Ui.column(this);
        root.setBackgroundColor(Ui.BG);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
            v.setPadding(bars.left + Ui.dp(this, 16), bars.top + Ui.dp(this, 8),
                    bars.right + Ui.dp(this, 16), bars.bottom);
            return insets;
        });
        setContentView(root);
        Button back = Ui.button(this,
                "‹  " + getString(global ? R.string.global_options : R.string.game_options), false);
        back.setOnClickListener(v -> finish());
        root.addView(back, Ui.space(this, 48, 8));
        Button reset = Ui.button(this, getString(R.string.reset_all_options), false);
        reset.setId(R.id.options_reset_all);
        reset.setEnabled(false);
        reset.setOnClickListener(v -> {
            if (saving || settings == null)
                return;
            TextView error = Ui.text(this, "", 13, 0xFFFFB4AB, false);
            error.setPadding(Ui.dp(this, 20), 0, Ui.dp(this, 20), 0);
            AlertDialog dialog = new AlertDialog.Builder(this)
                                         .setTitle(R.string.reset_all_options)
                                         .setMessage(global ? R.string.reset_global_confirmation
                                                            : R.string.reset_game_confirmation)
                                         .setView(error)
                                         .setPositiveButton(R.string.options_reset, null)
                                         .setNegativeButton(android.R.string.cancel, null)
                                         .create();
            dialog.setOnShowListener(d
                    -> dialog.getButton(-1).setOnClickListener(
                            button -> save(dialog, error, null, null)));
            dialog.show();
        });
        root.addView(reset, Ui.space(this, 48, 8));
        if (!global)
            root.addView(Ui.text(this, getIntent().getStringExtra("game_name"), 20, Ui.TEXT, true));
        root.addView(Ui.text(this,
                getString(global ? R.string.global_explanation : R.string.options_explanation), 13,
                Ui.MUTED, false));
        search = new EditText(this);
        search.setId(R.id.options_search);
        search.setSingleLine(true);
        search.setHint(R.string.options_search);
        root.addView(search);
        categories = new Spinner(this);
        root.addView(categories);
        status = Ui.text(this, getString(R.string.info_loading), 13, Ui.MUTED, false);
        root.addView(status);
        list = new ListView(this);
        list.setId(R.id.options_list);
        root.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        list.setOnItemClickListener((p, v, pos, id) -> {
            if (!saving)
                edit(visible.get(pos));
        });
        categories.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                filter();
            }
            public void onNothingSelected(AdapterView<?> p) {}
        });
        search.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int count, int after) {}
            public void onTextChanged(CharSequence s, int a, int before, int count) {
                filter();
            }
            public void afterTextChanged(Editable e) {}
        });
        if (state != null)
            search.setText(state.getString("search", ""));
        worker.execute(() -> {
            try {
                String path = getIntent().getStringExtra("game_path");
                if (global) {
                    settings = new GameSettings(this, 0);
                } else if (getIntent().getIntExtra("title_id", 0) != 0) {
                    settings = new GameSettings(this, getIntent().getIntExtra("title_id", 0));
                } else {
                    File game = path.startsWith("content://")
                            ? AndroidStorage.file(this, Uri.parse(path))
                            : new File(path);
                    int[] info = GameIcons.metadataNative(game.getAbsolutePath());
                    if (info == null || info.length == 0 || info[0] == 0)
                        throw new java.io.IOException(getString(R.string.options_no_title));
                    settings = new GameSettings(this, info[0]);
                }
                String[][] data = settings.load();
                runOnUiThread(() -> {
                    if (isFinishing() || isDestroyed())
                        return;
                    options = data;
                    reset.setEnabled(true);
                    TreeSet<String> names = new TreeSet<>();
                    for (String[] row : data) names.add(row[1]);
                    List<String> items = new ArrayList<>();
                    items.add(getString(R.string.options_all));
                    items.addAll(names);
                    categories.setAdapter(new ArrayAdapter<>(
                            this, android.R.layout.simple_spinner_dropdown_item, items));
                    if (state != null)
                        categories.setSelection(
                                Math.min(state.getInt("category"), items.size() - 1));
                    status.setText("");
                    filter();
                });
            } catch (Exception e) {
                failure(e);
            }
        });
    }
    private void filter() {
        String query = search.getText().toString().toLowerCase(Locale.ROOT).trim();
        String category = categories.getSelectedItemPosition() > 0
                ? categories.getSelectedItem().toString()
                : "";
        visible.clear();
        List<Map<String, String>> items = new ArrayList<>();
        for (String[] row : options) {
            if (!category.isEmpty() && !category.equals(row[1]))
                continue;
            if (!(row[0] + " " + row[1] + " " + row[2]).toLowerCase(Locale.ROOT).contains(query))
                continue;
            visible.add(row);
            Map<String, String> item = new HashMap<>();
            item.put("name", row[0]);
            item.put("value",
                    row[1] + " · " + row[4] + " · "
                            + getString(row[6].equals("true")
                                            ? (global ? R.string.options_inherited
                                                      : R.string.options_custom)
                                            : (global ? R.string.default_value
                                                      : R.string.options_inherited)));
            items.add(item);
        }
        list.setAdapter(new SimpleAdapter(this, items, android.R.layout.simple_list_item_2,
                new String[] {"name", "value"},
                new int[] {android.R.id.text1, android.R.id.text2}));
    }
    private void edit(String[] option) {
        if (option[0].equals("ui_locale")) {
            AppLanguage.choose(this);
            return;
        }
        LinearLayout form = Ui.column(this);
        int pad = Ui.dp(this, 20);
        form.setPadding(pad, 0, pad, 0);
        form.addView(Ui.text(this, option[2], 14, Ui.TEXT, false));
        form.addView(
                Ui.text(this, getString(R.string.options_base, option[5]), 13, Ui.MUTED, false));
        List<String> choices = new ArrayList<>();
        if (option[3].equals("bool"))
            choices.addAll(Arrays.asList("false", "true"));
        else if (option.length > 8)
            choices.addAll(Arrays.asList(option).subList(8, option.length));
        Spinner choice = new Spinner(this);
        EditText value = new EditText(this);
        value.setId(R.id.option_value);
        value.setSingleLine(true);
        if (choices.isEmpty()) {
            value.setText(option[4]);
            if (option[3].equals("integer") || option[3].equals("double"))
                value.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED
                        | (option[3].equals("double") ? InputType.TYPE_NUMBER_FLAG_DECIMAL : 0));
            form.addView(value);
            if (option[3].equals("path")) {
                Button browse = Ui.button(this, getString(R.string.browse_path), false);
                browse.setOnClickListener(v
                        -> new AlertDialog.Builder(this)
                                .setTitle(R.string.browse_path)
                                .setItems(new String[] {getString(R.string.info_file),
                                                  getString(R.string.game_folder)},
                                        (d, which) -> {
                                            pathTarget = value;
                                            pathDirectory = which == 1;
                                            android.content
                                                    .Intent intent = new android.content.Intent(
                                                    pathDirectory
                                                            ? android.content.Intent
                                                                      .ACTION_OPEN_DOCUMENT_TREE
                                                            : option[0].equals("log_file")
                                                            ? android.content.Intent
                                                                      .ACTION_CREATE_DOCUMENT
                                                            : android.content.Intent
                                                                      .ACTION_OPEN_DOCUMENT);
                                            if (!pathDirectory)
                                                intent.setType("*/*").addCategory(
                                                        android.content.Intent.CATEGORY_OPENABLE);
                                            startActivityForResult(intent, 7302);
                                        })
                                .setNegativeButton(android.R.string.cancel, null)
                                .show());
                form.addView(browse);
            }
        } else {
            choice.setAdapter(new ArrayAdapter<>(
                    this, android.R.layout.simple_spinner_dropdown_item, choices));
            choice.setSelection(Math.max(0, choices.indexOf(option[4])));
            form.addView(choice);
        }
        TextView error = Ui.text(this, "", 13, 0xFFFFB4AB, false);
        form.addView(error);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(form);
        AlertDialog dialog = new AlertDialog.Builder(this)
                                     .setTitle(option[0])
                                     .setView(scroll)
                                     .setPositiveButton(R.string.options_save, null)
                                     .setNeutralButton(R.string.options_reset, null)
                                     .setNegativeButton(android.R.string.cancel, null)
                                     .create();
        dialog.setOnShowListener(d -> {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE)
                    .setOnClickListener(v
                            -> save(dialog, error, option[0],
                                    choices.isEmpty() ? value.getText().toString()
                                                      : choice.getSelectedItem().toString()));
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL)
                    .setOnClickListener(v -> save(dialog, error, option[0], null));
        });
        dialog.show();
    }
    @Override
    protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        EditText target = pathTarget;
        pathTarget = null;
        if (request != 7302 || result != RESULT_OK || target == null || !target.isAttachedToWindow()
                || data == null || data.getData() == null)
            return;
        try {
            File file = pathDirectory ? AndroidStorage.directory(this, data.getData())
                                      : AndroidStorage.file(this, data.getData());
            target.setText(file.getAbsolutePath());
        } catch (java.io.IOException e) {
            target.setError(e.getMessage());
        }
    }
    private void save(AlertDialog dialog, TextView error, String name, String value) {
        saving = true;
        for (int button : new int[] {-1, -2, -3})
            if (dialog.getButton(button) != null)
                dialog.getButton(button).setEnabled(false);
        dialog.setCancelable(false);
        worker.execute(() -> {
            try {
                if (name == null)
                    settings.reset();
                else
                    settings.save(name, value);
                String[][] data = settings.load();
                runOnUiThread(() -> {
                    saving = false;
                    if (isFinishing() || isDestroyed())
                        return;
                    options = data;
                    dialog.dismiss();
                    status.setText(R.string.options_saved);
                    filter();
                    // Reset may also clear the global locale override.
                    if (name == null && global)
                        recreate();
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    saving = false;
                    if (isFinishing() || isDestroyed())
                        return;
                    error.setText(getString(R.string.options_error, e.getMessage()));
                    for (int button : new int[] {-1, -2, -3})
                        if (dialog.getButton(button) != null)
                            dialog.getButton(button).setEnabled(true);
                    dialog.setCancelable(true);
                });
            }
        });
    }
    private void failure(Exception e) {
        runOnUiThread(() -> {
            if (!isFinishing() && !isDestroyed())
                status.setText(getString(R.string.options_error, e.getMessage()));
        });
    }
    @Override
    protected void onSaveInstanceState(Bundle out) {
        out.putString("search", search.getText().toString());
        out.putInt("category", Math.max(0, categories.getSelectedItemPosition()));
        super.onSaveInstanceState(out);
    }
}
