package jp.xenia.emulator;

import java.io.*;
import java.nio.channels.*;

/** Prevents competing emulator sessions or external writers from overwriting live data. */
final class DataLock implements AutoCloseable {
    private final FileChannel channel;
    private final FileLock lock;
    DataLock(File root) throws IOException {
        root.mkdirs();
        channel = new RandomAccessFile(new File(root, ".data.lock"), "rw").getChannel();
        FileLock acquired;
        try {
            acquired = channel.tryLock();
        } catch (OverlappingFileLockException e) {
            acquired = null;
        } catch (IOException | RuntimeException e) {
            channel.close();
            throw e;
        }
        if (acquired == null) {
            channel.close();
            throw new IOException(
                    "Close the game or management screen before changing these files.");
        }
        lock = acquired;
    }
    @Override
    public void close() throws IOException {
        try {
            lock.release();
        } finally {
            channel.close();
        }
    }
}
