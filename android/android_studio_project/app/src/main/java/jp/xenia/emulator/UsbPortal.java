package jp.xenia.emulator;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;

/** Grants a borrowed USB descriptor to the same libusb portal backend as desktop. */
final class UsbPortal implements AutoCloseable {
    private final Activity activity;
    private final UsbManager manager;
    private final long context;
    private final String permission;
    private UsbDeviceConnection connection;
    private String deviceName;
    private final java.util.HashSet<String> requested = new java.util.HashSet<>();
    private static native boolean attachNative(long context, int fd);
    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (device == null)
                return;
            if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction())) {
                requested.remove(device.getDeviceName());
                if (device.getDeviceName().equals(deviceName))
                    detach();
            } else if (permission.equals(intent.getAction())) {
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
                    open(device);
            } else
                scan();
        }
    };
    UsbPortal(Activity activity, long context) {
        this.activity = activity;
        this.context = context;
        manager = activity.getSystemService(UsbManager.class);
        permission = activity.getPackageName() + ".USB_PORTAL";
        IntentFilter filter = new IntentFilter(permission);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (android.os.Build.VERSION.SDK_INT >= 33)
            activity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else
            activity.registerReceiver(receiver, filter);
        scan();
    }
    private void scan() {
        if (connection != null || manager == null)
            return;
        for (UsbDevice device : manager.getDeviceList().values()) {
            if (!supported(device.getVendorId(), device.getProductId()))
                continue;
            if (manager.hasPermission(device)) {
                open(device);
                return;
            }
            if (requested.add(device.getDeviceName())) {
                manager.requestPermission(device,
                        PendingIntent.getBroadcast(activity, 0,
                                new Intent(permission).setPackage(activity.getPackageName()),
                                PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
                return;
            }
        }
    }
    static boolean supported(int vendor, int product) {
        return vendor == 0x1430 && product == 0x1F17 || vendor == 0x24C6 && product == 0xFA00;
    }
    private void open(UsbDevice device) {
        if (connection != null || !supported(device.getVendorId(), device.getProductId())
                || !manager.hasPermission(device))
            return;
        UsbDeviceConnection candidate = manager.openDevice(device);
        if (candidate == null)
            return;
        if (!attachNative(context, candidate.getFileDescriptor())) {
            candidate.close();
            android.widget.Toast
                    .makeText(activity, R.string.portal_failed, android.widget.Toast.LENGTH_LONG)
                    .show();
            return;
        }
        connection = candidate;
        deviceName = device.getDeviceName();
    }
    void rebind() {
        if (connection != null)
            attachNative(context, connection.getFileDescriptor());
    }
    private void detach() {
        attachNative(context, -1);
        if (connection != null)
            connection.close();
        connection = null;
        deviceName = null;
    }
    @Override
    public void close() {
        activity.unregisterReceiver(receiver);
        detach();
    }
}
