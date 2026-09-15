package jp.xenia.emulator;

import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;
import android.text.format.DateFormat;
import android.text.format.Formatter;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Read-only title details, corresponding to the desktop Info panel. */
public class GameInfoActivity extends LocalizedActivity {
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private GameIcons icons;

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        GameLibrary.Game selected = null;
        GameLibrary library = new GameLibrary(this);
        for (GameLibrary.Game game : library.games)
            if (game.id.equals(getIntent().getStringExtra("game_id")))
                selected = game;
        if (selected == null) {
            finish();
            return;
        }
        final GameLibrary.Game game = selected;
        icons = new GameIcons(this);
        getWindow().setStatusBarColor(Ui.BG);
        getWindow().setNavigationBarColor(Ui.BG);
        LinearLayout root = Ui.column(this);
        root.setBackgroundColor(Ui.BG);
        Ui.applyInsets(root, 20, 12);
        setContentView(root);
        Button back = Ui.button(this, "‹  " + getString(R.string.back_library), false);
        back.setOnClickListener(v -> finish());
        root.addView(back, Ui.space(this, 48, 12));
        ScrollView scroll = new ScrollView(this);
        scroll.setId(R.id.game_info_scroll);
        LinearLayout content = Ui.column(this);
        content.setPadding(0, 0, 0, Ui.dp(this, 24));
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout.LayoutParams artSize = new LinearLayout.LayoutParams(Ui.dp(this, 180), -2);
        artSize.gravity = Gravity.CENTER_HORIZONTAL;
        artSize.bottomMargin = Ui.dp(this, 18);
        content.addView(new GameArtwork(this, icons, game), artSize);
        TextView title = Ui.text(this, game.name, 26, Ui.TEXT, true);
        title.setGravity(Gravity.CENTER);
        title.setTextIsSelectable(true);
        content.addView(title, Ui.space(this, -2, 24));

        section(content, R.string.game_information);
        String unknown = getString(R.string.info_unavailable);
        TextView titleId = field(content, R.string.info_title_id,
                game.titleId == 0 ? unknown : String.format(Locale.ROOT, "%08X", game.titleId));
        TextView mediaId = field(content, R.string.info_media_id,
                game.mediaId == 0 ? unknown : String.format(Locale.ROOT, "%08X", game.mediaId));
        TextView version = field(content, R.string.info_version,
                game.version == 0 ? unknown : version(game.version));
        TextView baseVersion = field(content, R.string.info_base_version,
                game.baseVersion == 0 ? unknown : version(game.baseVersion));
        TextView disc = field(content, R.string.info_disc,
                game.disc > 0 && game.discCount >= game.disc
                        ? getString(R.string.info_disc_number, game.disc, game.discCount) : unknown);
        long played = library.lastPlayed(game);
        field(content, R.string.info_last_played,
                played == 0 ? getString(R.string.info_never)
                            : DateFormat.getDateFormat(this).format(new Date(played)) + " · "
                                + DateFormat.getTimeFormat(this).format(new Date(played)));
        TextView compatibility = field(
                content, R.string.desktop_compatibility, getString(R.string.info_unavailable));
        section(content, R.string.info_file);
        field(content, R.string.info_format, game.format);
        TextView size = field(content, R.string.info_size, unknown);
        field(content, R.string.info_path, game.path);
        String[] pages = {"content", "saves", "patches", "stats"};
        int[] labels = {R.string.content, R.string.saves, R.string.patches, R.string.statistics};
        for (int i = 0; i < pages.length; ++i) {
            final String page = pages[i];
            Button action = Ui.button(this, getString(labels[i]), false);
            action.setOnClickListener(v
                    -> startActivity(game.identify(
                            new android.content.Intent(this, ToolsActivity.class)
                                    .putExtra("page", page))));
            content.addView(action, Ui.space(this, 48, 8));
        }
        TextView status = Ui.text(this, getString(R.string.info_loading), 13, Ui.MUTED, false);
        content.addView(status, Ui.space(this, -2, 12));
        // Only small headers are read here; the UI remains usable on slow storage.
        io.execute(() -> {
            long bytes = -1;
            int[] metadata = null;
            int compat = game.compatibility;
            boolean readable = false;
            try {
                File file = game.path.startsWith("content:")
                        ? AndroidStorage.file(this, Uri.parse(game.path))
                        : new File(game.path);
                readable = file.isFile() && file.canRead();
                if (readable) {
                    bytes = file.length();
                    metadata = GameIcons.readMetadata(file.getAbsolutePath());
                    if (metadata != null && metadata.length > 0 && metadata[0] != 0)
                        compat = GameIcons.compatibilityNative(metadata[0]);
                }
            } catch (Exception | LinkageError ignored) {
            }
            final long fileBytes = bytes;
            final int[] info = metadata;
            final int compatibilityState = compat;
            final boolean available = readable;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed())
                    return;
                if (fileBytes >= 0)
                    size.setText(Formatter.formatFileSize(this, fileBytes));
                String[] states = getResources().getStringArray(R.array.compat_states);
                compatibility.setText(
                        states[compatibilityState >= 0 && compatibilityState < states.length
                                        ? compatibilityState
                                        : 0]);
                if (info != null && info.length == 6) {
                    if (info[0] != 0)
                        titleId.setText(String.format(Locale.ROOT, "%08X", info[0]));
                    if (info[1] != 0)
                        mediaId.setText(String.format(Locale.ROOT, "%08X", info[1]));
                    version.setText(version(info[2]));
                    if (info[3] != 0)
                        baseVersion.setText(version(info[3]));
                    if (info[4] > 0 && info[5] >= info[4])
                        disc.setText(getString(R.string.info_disc_number, info[4], info[5]));
                    status.setVisibility(View.GONE);
                } else
                    status.setText(
                            available ? R.string.info_no_metadata : R.string.file_unavailable);
            });
        });
    }

    private static String version(int packed) {
        return String.format(Locale.ROOT, "%d.%d.%d.%d", packed >>> 28, (packed >>> 24) & 15,
                (packed >>> 8) & 65535, packed & 255);
    }
    private void section(LinearLayout content, int label) {
        TextView heading = Ui.text(this, getString(label), 18, Ui.TEXT, true);
        heading.setPadding(0, Ui.dp(this, 10), 0, 0);
        content.addView(heading, Ui.space(this, -2, 16));
    }
    private TextView field(LinearLayout content, int label, String value) {
        content.addView(
                Ui.text(this, getString(label), 12, Ui.MUTED, false), Ui.space(this, -2, 4));
        TextView text = Ui.text(this, value, 16, Ui.TEXT, false);
        text.setTextIsSelectable(true);
        content.addView(text, Ui.space(this, -2, 16));
        return text;
    }
    @Override
    protected void onDestroy() {
        io.shutdownNow();
        if (icons != null)
            icons.close();
        super.onDestroy();
    }
}
