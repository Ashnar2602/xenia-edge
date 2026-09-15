package jp.xenia.emulator;

import android.app.Activity;
import android.os.Bundle;
import java.io.File;

/** Native folder actions enter the same scoped document provider as Java menus. */
public final class FilesActivity extends LocalizedActivity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            File path = new File(getIntent().getStringExtra("path")).getCanonicalFile();
            AndroidStorage.openDirectory(this, path);
            finish();
        } catch (Exception e) {
            new android.app.AlertDialog.Builder(this)
                    .setMessage(e.getMessage())
                    .setPositiveButton(android.R.string.ok, (d, w) -> finish())
                    .setOnCancelListener(d -> finish())
                    .show();
        }
    }
}
