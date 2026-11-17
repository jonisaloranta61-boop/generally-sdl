package com.winlator;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.PreferenceManager;

import com.winlator.container.Container;
import com.winlator.container.ContainerManager;
import com.winlator.core.FileUtils;
import com.winlator.inputcontrols.InputControlsManager;
import com.winlator.xenvironment.RootFS;
import com.winlator.xenvironment.RootFSInstaller;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.util.concurrent.Executors;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** GeneRally-paketti: ensimmäisellä käynnistyksellä luo kontin, purkaa pelin, asentaa napit ja käynnistää pelin. */
public abstract class AutoGame {
    private static final int PACK_VERSION = 1;
    private static final int PROFILE_ID = 100;
    private static boolean showing = false;
    private static boolean showingCrash = false;
    private static boolean handlerSet = false;

    /** Tallentaa Java-kaatumisen tiedostoon ja näyttää sen seuraavalla käynnistyksellä. */
    private static void installCrashLog(final MainActivity activity) {
        final File f = new File(activity.getFilesDir(), "gr_crash.txt");
        if (f.isFile() && !showingCrash) {
            showingCrash = true;
            try {
                java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(f));
                StringBuilder sb = new StringBuilder();
                String line; int n = 0;
                while ((line = br.readLine()) != null && n++ < 25) sb.append(line).append('\n');
                br.close();
                f.delete();
                new AlertDialog.Builder(activity).setTitle("Edellinen kaatuminen")
                    .setMessage(sb.toString()).setPositiveButton("OK", null).show();
            } catch (Exception e) {}
        }
        if (handlerSet) return;
        handlerSet = true;
        final Thread.UncaughtExceptionHandler old = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try {
                StringWriter sw = new StringWriter();
                e.printStackTrace(new PrintWriter(sw));
                FileUtils.writeString(f, "Saie: " + t.getName() + "\n" + sw);
            } catch (Throwable x) {}
            if (old != null) old.uncaughtException(t, e);
        });
    }

    public static void begin(final MainActivity activity) {
        installCrashLog(activity);
        if (showing) return;
        RootFS rootFS = RootFS.find(activity);
        if (!rootFS.isValid() || rootFS.getVersion() < RootFSInstaller.LATEST_VERSION) return; // asennus kutsuu uudestaan
        showing = true;

        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(activity);
        final EditText et = new EditText(activity);
        et.setSingleLine(true);
        et.setInputType(InputType.TYPE_CLASS_TEXT);
        et.setHint("esim. kisa123");
        et.setText(prefs.getString("gr_room", "kokeilu1"));

        LinearLayout layout = new LinearLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = (int)(20 * activity.getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad / 2, pad, 0);
        TextView tv = new TextView(activity);
        tv.setText("Huonekoodi (sama kummallakin pelaajalla):");
        layout.addView(tv);
        layout.addView(et);

        new AlertDialog.Builder(activity)
            .setTitle("GeneRally Online")
            .setView(layout)
            .setCancelable(false)
            .setPositiveButton("Liity huoneeseen", (d, w) -> go(activity, et.getText().toString(), true))
            .setNegativeButton("Yksin (ei verkkoa)", (d, w) -> go(activity, et.getText().toString(), false))
            .show();
    }

    private static void go(final MainActivity activity, String roomRaw, final boolean online) {
        try { go2(activity, roomRaw, online); }
        catch (Throwable e) {
            activity.preloaderDialog.closeOnUiThread();
            showing = false;
            StringWriter sw = new StringWriter();
            e.printStackTrace(new PrintWriter(sw));
            String m = sw.toString();
            new AlertDialog.Builder(activity).setTitle("Virhe")
                .setMessage(m.length() > 1500 ? m.substring(0, 1500) : m).setPositiveButton("OK", null).show();
        }
    }

    private static void go2(final MainActivity activity, String roomRaw, final boolean online) {
        final String room = roomRaw.replaceAll("[^A-Za-z0-9_-]", "");
        final String roomFinal = room.isEmpty() ? "kokeilu1" : (room.length() > 24 ? room.substring(0, 24) : room);
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(activity);
        prefs.edit().putString("gr_room", roomFinal).apply();
        activity.preloaderDialog.show(R.string.loading);

        final ContainerManager manager = new ContainerManager(activity);
        int cid = prefs.getInt("gr_container_id", 0);
        Container existing = cid > 0 ? manager.getContainerById(cid) : null;
        if (existing != null) {
            prepare(activity, existing, roomFinal, online);
            return;
        }

        try {
            JSONObject data = new JSONObject();
            data.put("name", "GeneRally");
            data.put("screenSize", "800x600");
            manager.createContainerAsync(data, (container) -> {
                if (container == null) {
                    activity.preloaderDialog.closeOnUiThread();
                    showing = false;
                    new AlertDialog.Builder(activity).setMessage("Kontin luonti epäonnistui").setPositiveButton("OK", null).show();
                    return;
                }
                prefs.edit().putInt("gr_container_id", container.id).apply();
                prepare(activity, container, roomFinal, online);
            });
        }
        catch (Exception e) {
            activity.preloaderDialog.closeOnUiThread();
            showing = false;
        }
    }

    private static void prepare(final MainActivity activity, final Container container, final String room, final boolean online) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(activity);
        Executors.newSingleThreadExecutor().execute(() -> {
            boolean ok = true;
            String err = "";
            try {
                File gameDir = new File(container.getRootDir(), ".wine/drive_c/GeneRally");
                if (prefs.getInt("gr_pack_version", 0) != PACK_VERSION || !new File(gameDir, "GeneRally.exe").isFile()) {
                    FileUtils.delete(gameDir);
                    gameDir.mkdirs();
                    extractZip(activity, "generally.zip", gameDir);
                    prefs.edit().putInt("gr_pack_version", PACK_VERSION).apply();
                }

                String cfg = online
                    ? "mode=net\nroom=" + room + "\nbroker=broker.hivemq.com\ndelay=8\nwait=1800\n"
                      + "remap=119:273,115:274,97:276,100:275,264:0,265:0\n"
                    : "mode=off\nseed=0\nclock=real\naudio=on\n";
                FileUtils.writeString(new File(gameDir, "gr_net.cfg"), cfg);

                File profile = new File(InputControlsManager.getProfilesDir(activity), "controls-" + PROFILE_ID + ".icp");
                FileUtils.copy(activity, "gr_profile.icp", profile);

                File desktopDir = new File(container.getUserDir(), "Desktop");
                desktopDir.mkdirs();
                String desktop = "[Desktop Entry]\n"
                    + "Name=GeneRally\n"
                    + "Exec=env WINEPREFIX=\"/home/" + RootFS.USER + "/.wine\" wine C:\\\\\\\\GeneRally\\\\\\\\GeneRally.exe\n"
                    + "Type=Application\n"
                    + "StartupWMClass=generally\n"
                    + "\n[Extra Data]\n"
                    + "controlsProfile=" + PROFILE_ID + "\n";
                FileUtils.writeString(new File(desktopDir, "GeneRally.desktop"), desktop);
            }
            catch (Throwable e) {
                ok = false;
                StringWriter sw = new StringWriter();
                e.printStackTrace(new PrintWriter(sw));
                err = sw.toString();
            }

            final boolean success = ok;
            final String errF = err.length() > 1500 ? err.substring(0, 1500) : err;
            activity.runOnUiThread(() -> {
                activity.preloaderDialog.closeOnUiThread();
                showing = false;
                if (!success) {
                    new AlertDialog.Builder(activity).setMessage("Pelin valmistelu epäonnistui\n" + errF).setPositiveButton("OK", null).show();
                    return;
                }
                Intent intent = new Intent(activity, XServerDisplayActivity.class);
                intent.putExtra("container_id", container.id);
                intent.putExtra("shortcut_path", new File(container.getUserDir(), "Desktop/GeneRally.desktop").getPath());
                intent.putExtra("auto_game", true);
                activity.startActivity(intent);
                activity.finish();
            });
        });
    }

    private static void extractZip(Context context, String asset, File dst) throws Exception {
        String canon = dst.getCanonicalPath();
        byte[] buf = new byte[65536];
        try (InputStream is = context.getAssets().open(asset); ZipInputStream zis = new ZipInputStream(is)) {
            ZipEntry e;
            while ((e = zis.getNextEntry()) != null) {
                File out = new File(dst, e.getName());
                if (!out.getCanonicalPath().startsWith(canon)) continue;
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (FileOutputStream fos = new FileOutputStream(out)) {
                    int n;
                    while ((n = zis.read(buf)) > 0) fos.write(buf, 0, n);
                }
            }
        }
    }
}
