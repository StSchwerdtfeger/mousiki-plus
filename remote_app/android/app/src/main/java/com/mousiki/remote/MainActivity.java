package com.mousiki.remote;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * Mousiki Remote: a full-screen web view.
 *
 * The screen of the remote is drawn by Mousiki itself (its web server, Settings -> ON/OFF -> MODES -> Remote App) and
 * painted by the page Mousiki serves, so the app always looks exactly like the Mousiki version it talks to. This class
 * only adds what a page cannot do on its own: the connect screen (address, port, PIN; assets/connect.html), finding
 * Mousiki in the home network, the back button, haptic feedback, keeping the screen on and opening the manual in the
 * browser.
 */
public class MainActivity extends Activity {

    private static final String CONNECT_PAGE = "file:///android_asset/connect.html";

    private WebView web;
    private SharedPreferences prefs;
    private boolean onRemote = false;      // the page of a Mousiki is shown (not the connect screen)
    private String remoteBase = "";        // "http://192.168.1.20:4747"

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("mousiki", MODE_PRIVATE);

        web = new WebView(this);
        web.setBackgroundColor(Color.BLACK);
        web.setOverScrollMode(View.OVER_SCROLL_NEVER);
        web.setVerticalScrollBarEnabled(false);
        web.setHorizontalScrollBarEnabled(false);
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);          // the page keeps its PIN and display choices
        s.setTextZoom(100);                    // the system font size must not change the grid
        s.setSupportZoom(false);
        s.setAllowFileAccess(true);            // assets/connect.html
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);

        web.addJavascriptInterface(new Bridge(), "MousikiApp");
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return leaveApp(request.getUrl().toString());
            }

            @SuppressWarnings("deprecation")
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, String url) {
                return leaveApp(url);
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (Build.VERSION.SDK_INT >= 23 && request.isForMainFrame() && onRemote)
                    showConnect("No answer from " + remoteBase.replace("http://", "")
                            + " -- is Mousiki running with Remote App on, in the same network?");
            }

            @SuppressWarnings("deprecation")
            @Override
            public void onReceivedError(WebView view, int errorCode, String description, String failingUrl) {
                if (Build.VERSION.SDK_INT < 23 && onRemote && failingUrl != null && failingUrl.startsWith(remoteBase))
                    showConnect("No answer from " + remoteBase.replace("http://", "")
                            + " -- is Mousiki running with Remote App on, in the same network?");
            }
        });

        applyKeepScreenOn(prefs.getBoolean("keep_on", true));

        String host = prefs.getString("host", "");
        String pin = prefs.getString("pin", "");
        if (!host.isEmpty() && !pin.isEmpty() && prefs.getBoolean("auto", true))
            connect(host, prefs.getString("port", "4747"), pin);
        else
            showConnect("");
    }

    // Links that are neither the Mousiki page nor the connect screen (the manual) open in the browser.
    private boolean leaveApp(String url) {
        if (url == null) return false;
        if (url.startsWith("file:///android_asset/")) return false;
        if (!remoteBase.isEmpty() && url.startsWith(remoteBase)) return false;
        openExternal(url);
        return true;
    }

    private void openExternal(String url) {
        if (url == null || !(url.startsWith("https://") || url.startsWith("http://"))) return;
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException ignored) {
        }
    }

    private void showConnect(String message) {
        onRemote = false;
        web.loadUrl(CONNECT_PAGE + (message == null || message.isEmpty() ? "" : "#" + Uri.encode(message)));
    }

    private void connect(String host, String port, String pin) {
        remoteBase = "http://" + host.trim() + ":" + port.trim();
        onRemote = true;
        web.loadUrl(remoteBase + "/#pin=" + Uri.encode(pin.trim()));
    }

    private void applyKeepScreenOn(boolean on) {
        if (on) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    public void onBackPressed() {
        if (!onRemote) {
            super.onBackPressed();
            return;
        }
        // the page closes its own panel first; otherwise back goes to the connect screen
        web.evaluateJavascript("(window.mousikiBack && window.mousikiBack()) ? 'yes' : 'no'", value -> {
            if (value == null || !value.contains("yes")) showConnect("");
        });
    }

    @Override
    protected void onPause() {
        super.onPause();
        web.onPause();       // the page stops asking for the screen while the app is in the background
    }

    @Override
    protected void onResume() {
        super.onResume();
        web.onResume();
    }

    // ---- what the pages may call ---------------------------------------------------------------------------------------
    private class Bridge {
        @JavascriptInterface
        public String get(String key) {
            switch (key) {
                case "host": return prefs.getString("host", "");
                case "port": return prefs.getString("port", "4747");
                case "pin": return prefs.getString("pin", "");
                case "keep_on": return prefs.getBoolean("keep_on", true) ? "true" : "false";
                case "auto": return prefs.getBoolean("auto", true) ? "true" : "false";
                default: return "";
            }
        }

        @JavascriptInterface
        public void setOption(String key, boolean on) {
            if (!key.equals("keep_on") && !key.equals("auto")) return;
            prefs.edit().putBoolean(key, on).apply();
            if (key.equals("keep_on")) runOnUiThread(() -> applyKeepScreenOn(on));
        }

        @JavascriptInterface
        public void connect(String host, String port, String pin) {
            prefs.edit().putString("host", host.trim()).putString("port", port.trim()).putString("pin", pin.trim()).apply();
            runOnUiThread(() -> MainActivity.this.connect(host, port, pin));
        }

        @JavascriptInterface
        public void connectScreen() {
            runOnUiThread(() -> showConnect(""));
        }

        @JavascriptInterface
        public void openUrl(String url) {
            runOnUiThread(() -> openExternal(url));
        }

        @JavascriptInterface
        public void haptic() {
            runOnUiThread(() -> web.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY));
        }

        /** Looks for Mousiki in the phone's own network (x.y.z.1 - 254) on `port`; answers window.mousikiFound([...]). */
        @JavascriptInterface
        public void scan(String port) {
            new Thread(() -> {
                List<String> found = findMousiki(port);
                StringBuilder js = new StringBuilder("window.mousikiFound && window.mousikiFound([");
                for (int i = 0; i < found.size(); i++) js.append(i > 0 ? "," : "").append('"').append(found.get(i)).append('"');
                js.append("])");
                runOnUiThread(() -> web.evaluateJavascript(js.toString(), null));
            }).start();
        }
    }

    // ---- finding Mousiki ---------------------------------------------------------------------------------------------------
    private static List<String> findMousiki(String portText) {
        final int port;
        try {
            port = Integer.parseInt(portText.trim());
        } catch (NumberFormatException e) {
            return new ArrayList<>();
        }
        List<String> prefixes = new ArrayList<>();
        try {
            Enumeration<NetworkInterface> nis = NetworkInterface.getNetworkInterfaces();
            while (nis != null && nis.hasMoreElements()) {
                NetworkInterface ni = nis.nextElement();
                if (!ni.isUp() || ni.isLoopback()) continue;
                Enumeration<InetAddress> as = ni.getInetAddresses();
                while (as.hasMoreElements()) {
                    InetAddress a = as.nextElement();
                    if (!(a instanceof Inet4Address) || !a.isSiteLocalAddress()) continue;
                    String ip = a.getHostAddress();
                    String prefix = ip.substring(0, ip.lastIndexOf('.') + 1);
                    if (!prefixes.contains(prefix)) prefixes.add(prefix);
                }
            }
        } catch (Exception ignored) {
        }
        final List<String> found = Collections.synchronizedList(new ArrayList<>());
        ExecutorService pool = Executors.newFixedThreadPool(48);
        for (String prefix : prefixes) {
            for (int i = 1; i <= 254; i++) {
                final String host = prefix + i;
                pool.execute(() -> {
                    if (isMousiki(host, port)) found.add(host);
                });
            }
        }
        pool.shutdown();
        try {
            pool.awaitTermination(15, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
        }
        List<String> out = new ArrayList<>(found);
        Collections.sort(out);
        return out;
    }

    // Mousiki's /api/hello answers {"ok":true,...} without the PIN too (it does not count as a wrong PIN); older versions answered {"error":"wrong PIN"}.
    private static boolean isMousiki(String host, int port) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL("http://" + host + ":" + port + "/api/hello?pin=0").openConnection();
            c.setConnectTimeout(700);
            c.setReadTimeout(1200);
            int code = c.getResponseCode();
            InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
            if (in == null) return false;
            byte[] buf = new byte[256];
            int n = in.read(buf);
            in.close();
            String body = n > 0 ? new String(buf, 0, n, "UTF-8") : "";
            return body.contains("\"ok\"") || body.contains("PIN");
        } catch (Exception e) {
            return false;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
