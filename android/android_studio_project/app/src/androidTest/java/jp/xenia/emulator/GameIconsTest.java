package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.Context;
import android.net.Uri;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class GameIconsTest {
    static {
        System.loadLibrary("xenia-app");
    }
    private static final byte[] PNG =
            android.util.Base64.decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP"
                                       + "4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==",
                    0);
    private static final byte[] RETAIL = {(byte) 0x20, (byte) 0xb1, (byte) 0x85, (byte) 0xa5,
            (byte) 0x9d, 0x28, (byte) 0xfd, (byte) 0xc3, 0x40, 0x58, 0x3f, (byte) 0xbb, 0x08,
            (byte) 0x96, (byte) 0xbf, (byte) 0x91};
    private static ByteBuffer buffer(int size) {
        return ByteBuffer.allocate(size);
    }
    private static byte[] encrypt(byte[] input, byte[] key) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/CBC/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"),
                new IvParameterSpec(new byte[16]));
        return cipher.doFinal(input);
    }
    // Synthetic resource-bearing executable. No commercial game data in tests.
    private static byte[] xex(int compression, byte[] key) throws Exception {
        ByteBuffer image = buffer(0x4000);
        image.put(0, (byte) 'M').put(1, (byte) 'Z');
        image.position(0x1000);
        image.putInt(0x58444246).putInt(0x10000).putInt(1).putInt(1).putInt(0).putInt(0);
        image.putShort((short) 2).putLong(0x8000).putInt(0).putInt(PNG.length).put(PNG);
        byte[] payload = image.array();
        ByteBuffer header = buffer(0x400);
        header.putInt(0x58455832).putInt(0).putInt(0x400).putInt(0).putInt(0x100).putInt(2);
        header.putInt(0x2ff).putInt(0x40).putInt(0x3ff).putInt(0x60);
        header.position(0x40);
        header.putInt(20).putLong(0x5445535449434f4eL).putInt(0x82001000).putInt(42 + PNG.length);
        header.putInt(0x104, image.capacity()).putInt(0x210, 0x82000000);
        header.position(0x60);
        header.putInt(compression == 0 ? 8 : compression == 1 ? 24 : 36);
        header.putShort((short) (key == null ? 0 : 1)).putShort((short) compression);
        if (compression == 1) {
            header.putInt(0x200).putInt(0xe00).putInt(0x3000).putInt(0);
            ByteBuffer basic = buffer(0x3200);
            basic.put(payload, 0, 0x200).put(payload, 0x1000, 0x3000);
            payload = basic.array();
        } else if (compression == 2) {
            // LZX uncompressed block: Intel flag, type 3, 24-bit length, word alignment.
            int bits = (3 << 28) | (payload.length << 4);
            ByteBuffer lzx = buffer(payload.length + 16).order(ByteOrder.LITTLE_ENDIAN);
            lzx.putShort((short) (bits >>> 16)).putShort((short) bits);
            lzx.putInt(1).putInt(1).putInt(1).put(payload);
            ByteBuffer block = buffer((24 + 2 + lzx.capacity() + 2 + 15) & ~15);
            block.position(24);
            block.putShort((short) lzx.capacity()).put(lzx.array()).putShort((short) 0);
            payload = block.array();
            header.putInt(32768)
                    .putInt(payload.length)
                    .put(MessageDigest.getInstance("SHA-1").digest(payload));
        }
        if (key != null) {
            byte[] session = new byte[16];
            Arrays.fill(session, (byte) 0x73);
            header.position(0x250);
            header.put(encrypt(session, key));
            payload = encrypt(payload, session);
        }
        ByteBuffer file = buffer(header.capacity() + payload.length);
        return file.put(header.array()).put(payload).array();
    }
    private static byte[] read(byte[] bytes, String suffix) throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File file = File.createTempFile("icon-fixture", suffix, context.getCacheDir());
        try {
            Files.write(file.toPath(), bytes);
            return GameIcons.extractNative(file.getAbsolutePath());
        } finally {
            assertTrue(file.delete());
        }
    }
    private static int[] readMetadata(byte[] bytes, String suffix) throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File file = File.createTempFile("metadata-fixture", suffix, context.getCacheDir());
        try {
            Files.write(file.toPath(), bytes);
            return GameIcons.metadataNative(file.getAbsolutePath());
        } finally { assertTrue(file.delete()); }
    }
    @Test public void executionInfoIsReadWithoutDecodingTheImage() throws Exception {
        byte[] file = xex(2, RETAIL);
        ByteBuffer header = ByteBuffer.wrap(file);
        header.putInt(20, 3).putInt(40, 0x40006).putInt(44, 0x80);
        header.position(0x80);
        header.putInt(0x12345678).putInt(0x10000203).putInt(0x10000000)
                .putInt(0x5553083b).put((byte)2).put((byte)0).put((byte)1).put((byte)2).putInt(0);
        int[] expected = {0x5553083b, 0x12345678, 0x10000203, 0x10000000, 1, 2};
        assertArrayEquals(expected, readMetadata(file, ".xex"));
        assertArrayEquals(expected, readMetadata(Arrays.copyOf(file, 0x400), ".xex"));
        header.putInt(44, 0x3ff);
        assertNull(readMetadata(file, ".xex"));
        header.putInt(20, Integer.MAX_VALUE);
        assertNull(readMetadata(file, ".xex"));
    }
    @Test
    public void xexCompressionAndEncryption() throws Exception {
        for (int compression = 0; compression <= 2; ++compression)
            for (byte[] key : new byte[][] {null, RETAIL, new byte[16]})
                assertArrayEquals("Compression " + compression + ", encrypted " + (key != null),
                        PNG, read(xex(compression, key), ".xex"));
        byte[] relocated = xex(0, null);
        ByteBuffer.wrap(relocated).putInt(20, 3).putInt(40, 0x10201)
                .putInt(44, 0x82000000).putInt(0x210, 0x81000000);
        assertArrayEquals(PNG, read(relocated, ".xex"));
        byte[] xex1 = xex(0, null);
        ByteBuffer.wrap(xex1).putInt(0, 0x58455831).putInt(0x230, 0x82000000);
        assertArrayEquals(PNG, read(xex1, ".xex"));
        assertNotNull(GameIcons.decode(PNG));
    }
    @Test
    public void isoFindsEmbeddedExecutable() throws Exception {
        byte[] xex = xex(2, RETAIL);
        ByteBuffer iso = buffer(36 * 2048 + xex.length).order(ByteOrder.LITTLE_ENDIAN);
        iso.position(32 * 2048);
        iso.put("MICROSOFT*XBOX*MEDIA".getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        iso.putInt(33).putInt(64);
        iso.position(33 * 2048);
        iso.putShort((short) 0).putShort((short) 0).putInt(36).putInt(xex.length);
        iso.put((byte) 0).put((byte) 11).put(
                "DEFAULT.XEX".getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        iso.position(36 * 2048);
        iso.put(xex);
        assertArrayEquals(PNG, read(iso.array(), ".iso"));
        // A corrupt tree referring back to itself must terminate.
        iso.putShort(33 * 2048, (short) 4).putShort(33 * 2048 + 2, (short) 4);
        iso.putShort(33 * 2048 + 16, (short) 4).putShort(33 * 2048 + 18, (short) 4);
        assertNull(read(Arrays.copyOf(iso.array(), 34 * 2048), ".iso"));
    }
    @Test
    public void zarFindsEmbeddedExecutable() throws Exception {
        byte[] xex = xex(2, RETAIL);
        int names = 65536 + 40, tree = names + 13, footer = tree + 32;
        ByteBuffer zar = buffer(footer + 144);
        zar.put(xex).position(65536);
        zar.putLong(0).putShort((short) 65535);
        zar.position(names);
        zar.put((byte) 0).put((byte) 11).put(
                "default.xex".getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        zar.position(tree);
        zar.putInt(0).putInt(1).putInt(1).putInt(0);
        zar.putInt(0x80000001).putInt(0).putInt(xex.length).putInt(0);
        zar.position(footer);
        zar.putLong(0).putLong(65536).putLong(65536).putLong(40);
        zar.putLong(names).putLong(13).putLong(tree).putLong(32);
        zar.position(footer + 128);
        zar.putLong(zar.capacity()).putInt(0x61bf3a01).putInt(0x169f52d6);
        assertArrayEquals(PNG, read(zar.array(), ".zar"));
        zar.putInt(tree + 16, 0x8000000d); // Name points past the name table.
        assertNull(read(zar.array(), ".zar"));
        zar.putLong(footer + 24, Long.MAX_VALUE);
        assertNull(read(zar.array(), ".zar"));
    }
    @Test
    public void stfsAndGodThumbnail() throws Exception {
        for (int magic : new int[] {0x434f4e20, 0x4c495645, 0x50495253}) {
            ByteBuffer container = buffer(0x971a);
            container.putInt(magic).putInt(0x348, 1);
            container.putInt(0x1716, PNG.length);
            container.position(0x571a);
            container.put(PNG);
            assertArrayEquals(PNG, read(container.array(), ".container"));
        }
    }
    @Test
    public void malformedAndMissingResources() throws Exception {
        byte[] valid = xex(0, null);
        for (int length : new int[] {0, 3, 23, 25, 100, 1023})
            assertNull(read(Arrays.copyOf(valid, length), ".xex"));
        for (int offset : new int[] {8, 16, 20, 28, 36, 0x40, 0x50, 0x104}) {
            byte[] broken = valid.clone();
            ByteBuffer.wrap(broken).putInt(offset, -1);
            assertNull("Offset " + offset, read(broken, ".xex"));
        }
        byte[] noIcon = valid.clone();
        ByteBuffer.wrap(noIcon).putLong(0x400 + 0x1000 + 26, 123);
        assertNull(read(noIcon, ".xex"));
        byte[] badHash = xex(2, RETAIL);
        badHash[0x70] ^= 1;
        assertNull(read(badHash, ".xex"));
    }
    @Test
    public void existingLibraryArtwork() throws Exception {
        org.junit.Assume.assumeTrue("true".equals(
                InstrumentationRegistry.getArguments().getString("check_library_icons")));
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        GameLibrary library = new GameLibrary(context);
        assertFalse(library.games.isEmpty());
        for (GameLibrary.Game game : library.games) {
            File file = game.path.startsWith("content:")
                    ? AndroidStorage.file(context, Uri.parse(game.path))
                    : new File(game.path);
            long size = file.length(), modified = file.lastModified();
            byte[] icon = GameIcons.extractNative(file.getAbsolutePath());
            assertNotNull(game.name, icon);
            assertNotNull(game.name, GameIcons.decode(icon));
            int[] metadata = GameIcons.metadataNative(file.getAbsolutePath());
            assertNotNull(game.name, metadata);
            assertEquals(6, metadata.length);
            assertNotEquals(game.name, 0, metadata[0]);
            assertEquals(size, file.length());
            assertEquals(modified, file.lastModified());
        }
    }
}
