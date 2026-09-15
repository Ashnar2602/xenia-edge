package jp.xenia.emulator;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

final class Ui {
    static final int BG = Color.rgb(15, 19, 25), CARD = Color.rgb(26, 33, 42);
    static final int TEXT = Color.rgb(239, 245, 249), MUTED = Color.rgb(155, 169, 185);
    static final int GREEN = Color.rgb(155, 224, 107), LINE = Color.rgb(48, 61, 74);
    static int dp(Context c, float n) {
        return Math.round(n * c.getResources().getDisplayMetrics().density);
    }
    static GradientDrawable shape(Context c, int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(c, radius));
        return d;
    }
    static TextView text(Context c, String value, int size, int color, boolean bold) {
        TextView v = new TextView(c);
        v.setText(value);
        v.setTextSize(size);
        v.setTextColor(color);
        if (bold)
            v.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        return v;
    }
    static Button button(Context c, String value, boolean primary) {
        Button b = new Button(c);
        b.setText(value);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTextColor(primary ? BG : TEXT);
        b.setMinHeight(dp(c, 48));
        b.setPadding(dp(c, 16), 0, dp(c, 16), 0);
        b.setBackground(new RippleDrawable(
                ColorStateList.valueOf(0x3344BB44), shape(c, primary ? GREEN : CARD, 14), null));
        return b;
    }
    static LinearLayout column(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }
    static LinearLayout row(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }
    static LinearLayout.LayoutParams space(Context c, int height, int bottom) {
        LinearLayout.LayoutParams p =
                new LinearLayout.LayoutParams(-1, height < 0 ? height : dp(c, height));
        p.bottomMargin = dp(c, bottom);
        return p;
    }
    private Ui() {}
}
