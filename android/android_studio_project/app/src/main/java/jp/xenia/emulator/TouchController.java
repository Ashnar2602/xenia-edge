package jp.xenia.emulator;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;

/** One controller, with independent pointer tracking for sticks and buttons. */
final class TouchController extends View {
    interface Listener {
        void input(int buttons, float lx, float ly, float rx, float ry, float lt, float rt);
    }
    private static final class Control {
        final String label;
        final int mask, kind;
        final RectF rect = new RectF();
        Control(String label, int mask, int kind) {
            this.label = label;
            this.mask = mask;
            this.kind = kind;
        }
    }
    private final ArrayList<Control> controls = new ArrayList<>();
    private final SparseArray<float[]> pointers = new SparseArray<>();
    private final SparseArray<Control> owners = new SparseArray<>();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Listener listener;
    TouchController(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        setContentDescription(context.getString(R.string.touch_controls));
        controls.add(new Control("L", 0, 1));
        controls.add(new Control("R", 0, 2));
        controls.add(new Control("A", 0x1000, 0));
        controls.add(new Control("B", 0x2000, 0));
        controls.add(new Control("X", 0x4000, 0));
        controls.add(new Control("Y", 0x8000, 0));
        controls.add(new Control("↑", 1, 0));
        controls.add(new Control("↓", 2, 0));
        controls.add(new Control("←", 4, 0));
        controls.add(new Control("→", 8, 0));
        controls.add(new Control("VIEW", 0x20, 0));
        controls.add(new Control("START", 0x10, 0));
        controls.add(new Control("LB", 0x100, 0));
        controls.add(new Control("RB", 0x200, 0));
        controls.add(new Control("LT", 0, 3));
        controls.add(new Control("RT", 0, 4));
        controls.add(new Control("L3", 0x40, 0));
        controls.add(new Control("R3", 0x80, 0));
    }
    private void place(int index, float x, float y, float radius) {
        controls.get(index).rect.set(x - radius, y - radius, x + radius, y + radius);
    }
    @Override
    protected void onSizeChanged(int width, int height, int oldw, int oldh) {
        float unit = Math.min(Ui.dp(getContext(), 24), height / 16f);
        float left = width * .12f, right = width * .88f;
        place(0, left, height * .53f, unit * 1.9f);
        place(1, width * .69f, height * .79f, unit * 1.6f);
        place(2, right, height * .65f, unit);
        place(3, right + unit * 2.1f, height * .51f, unit);
        place(4, right - unit * 2.1f, height * .51f, unit);
        place(5, right, height * .37f, unit);
        float dx = width * .30f, dy = height * .78f, r = unit * .85f;
        place(6, dx, dy - unit * 1.7f, r);
        place(7, dx, dy + unit * 1.7f, r);
        place(8, dx - unit * 1.7f, dy, r);
        place(9, dx + unit * 1.7f, dy, r);
        place(10, width * .45f, height * .86f, unit);
        place(11, width * .55f, height * .86f, unit);
        place(12, width * .08f, height * .19f, unit);
        place(13, width * .92f, height * .19f, unit);
        place(14, width * .20f, height * .19f, unit);
        place(15, width * .80f, height * .19f, unit);
        place(16, width * .08f, height * .84f, unit * .8f);
        place(17, width * .82f, height * .84f, unit * .8f);
        reset();
    }
    private boolean down(Control c) {
        for (int i = 0; i < owners.size(); ++i)
            if (owners.valueAt(i) == c)
                return true;
        return false;
    }
    @Override
    protected void onDraw(Canvas canvas) {
        for (Control c : controls) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(down(c) ? 0x8877CC55 : 0x55202A34);
            canvas.drawOval(c.rect, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Ui.dp(getContext(), 1));
            paint.setColor(0x99FFFFFF);
            canvas.drawOval(c.rect, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xDDEAF1F5);
            paint.setTextSize(Ui.dp(getContext(), c.label.length() > 2 ? 10 : 16));
            paint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(c.label, c.rect.centerX(),
                    c.rect.centerY() - (paint.ascent() + paint.descent()) / 2, paint);
            if (c.kind == 1 || c.kind == 2) {
                float x = c.rect.centerX(), y = c.rect.centerY();
                for (int i = 0; i < owners.size(); ++i)
                    if (owners.valueAt(i) == c) {
                        float[] p = pointers.get(owners.keyAt(i));
                        if (p != null) {
                            x += clamp((p[0] - x) / (c.rect.width() / 2)) * c.rect.width() / 3;
                            y += clamp((p[1] - y) / (c.rect.height() / 2)) * c.rect.height() / 3;
                        }
                    }
                paint.setColor(0x88AFC6B5);
                canvas.drawCircle(x, y, c.rect.width() / 5, paint);
            }
        }
    }
    static float clamp(float n) {
        return Math.max(-1, Math.min(1, n));
    }
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex(),
            id = event.getPointerId(index);
        if (action == MotionEvent.ACTION_CANCEL) {
            reset();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = event.getX(index), y = event.getY(index);
            for (Control c : controls)
                if (c.rect.contains(x, y) && !down(c)) {
                    owners.put(id, c);
                    break;
                }
        }
        for (int i = 0; i < event.getPointerCount(); ++i)
            pointers.put(event.getPointerId(i), new float[] {event.getX(i), event.getY(i)});
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            owners.remove(id);
            pointers.remove(id);
        }
        publish();
        return true;
    }
    void reset() {
        pointers.clear();
        owners.clear();
        publish();
    }
    private void publish() {
        int buttons = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        for (int i = 0; i < owners.size(); ++i) {
            Control c = owners.valueAt(i);
            float[] p = pointers.get(owners.keyAt(i));
            if (p == null)
                continue;
            if (c.kind == 1 || c.kind == 2) {
                float x = clamp((p[0] - c.rect.centerX()) / (c.rect.width() / 2));
                float y = -clamp((p[1] - c.rect.centerY()) / (c.rect.height() / 2));
                if (c.kind == 1) {
                    lx = x;
                    ly = y;
                } else {
                    rx = x;
                    ry = y;
                }
            } else if (c.rect.contains(p[0], p[1])) {
                buttons |= c.mask;
                if (c.kind == 3)
                    lt = 1;
                if (c.kind == 4)
                    rt = 1;
            }
        }
        listener.input(buttons, lx, ly, rx, ry, lt, rt);
        invalidate();
    }
}
