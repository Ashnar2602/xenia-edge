package jp.xenia.emulator;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;
import java.util.Locale;

/** Square artwork viewport. FIT_CENTER preserves the entire embedded image. */
final class GameArtwork extends FrameLayout {
    GameArtwork(Context context, GameIcons icons, GameLibrary.Game game) {
        super(context);
        setBackground(Ui.shape(context, Ui.BG, 10));
        setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS);
        TextView fallback = Ui.text(context,
                game.name.isEmpty() ? "X" : game.name.substring(0, 1).toUpperCase(Locale.ROOT), 40,
                Ui.GREEN, true);
        fallback.setGravity(Gravity.CENTER);
        addView(fallback, new LayoutParams(-1, -1));
        ImageView image = new ImageView(context);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        addView(image, new LayoutParams(-1, -1));
        icons.bind(game.path, image, fallback);
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        super.onMeasure(widthSpec,
                MeasureSpec.makeMeasureSpec(MeasureSpec.getSize(widthSpec), MeasureSpec.EXACTLY));
    }
}
