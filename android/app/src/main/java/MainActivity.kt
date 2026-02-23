package com.altillimity.satdump

import android.app.NativeActivity
import android.os.Bundle
import android.os.Build
import android.os.SystemClock
import android.content.Context
import android.view.inputmethod.InputMethodManager
import java.util.concurrent.LinkedBlockingQueue
import android.util.Log
import android.content.res.AssetManager
import java.io.*
import java.util.zip.CRC32

import android.Manifest
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.DocumentsContract
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.EditText
import android.widget.RelativeLayout
import androidx.core.app.ActivityCompat
import androidx.core.content.PermissionChecker
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

// Extension on intent
fun Intent?.getFilePath(context: Context): String {
    return this?.data?.let { data -> RealPathUtil.getRealPath(context, data) ?: "" } ?: ""
}

// Extension on intent
fun Intent?.getFilePathDir(context: Context): String {
    return this?.data?.let { data -> RealPathUtil.getRealPath(context, DocumentsContract.buildDocumentUriUsingTree(data, DocumentsContract.getTreeDocumentId(data))) ?: "" } ?: ""
}

class MainActivity : NativeActivity(), TextWatcher {
    private val TAG : String = "SatDump";
    private val FORCED_ORIENTATION: Int = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
    private val PERMISSION_REQUEST_CODE = 1
    private val ASSET_SYNC_PREFS = "satdump_asset_sync"
    private val ASSET_SYNC_KEY = "asset_sync_version"

    data class AssetSyncStats(
        var filesChecked: Int = 0,
        var filesCopied: Int = 0
    )

    public var mLayout : ViewGroup? = null;
    public var editText : EditText? = null;
    public var lastFiller : String? = null;

    private fun requestMissingPermissionsIfNeeded() {
        val requiredPermissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            requiredPermissions.add(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            requiredPermissions.add(Manifest.permission.READ_EXTERNAL_STORAGE)
        }

        val missingPermissions = requiredPermissions.filter {
            PermissionChecker.checkSelfPermission(this, it) != PermissionChecker.PERMISSION_GRANTED
        }

        if (missingPermissions.isNotEmpty()) {
            ActivityCompat.requestPermissions(
                this,
                missingPermissions.toTypedArray(),
                PERMISSION_REQUEST_CODE
            )
        }
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = FORCED_ORIENTATION

        // Ask only once for missing permissions.
        requestMissingPermissionsIfNeeded()

        // Hide system bars
        val insetsController = WindowInsetsControllerCompat(window, window.decorView)
        insetsController.hide(WindowInsetsCompat.Type.systemBars())
        insetsController.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

        // Keep screen on
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Prevent Android from auto-showing IME when activity regains focus (e.g. after USB permission dialog)
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN)

        // Text crap
        mLayout = RelativeLayout(this);
        editText = EditText(this.applicationContext!!);
        mLayout!!.addView(editText, RelativeLayout.LayoutParams(10000, 10000));
        editText!!.setVisibility(View.VISIBLE);
        editText!!.setInputType(InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        editText!!.setText(" ");
        editText!!.setSelection(1);
        lastFiller = " ";
        editText!!.addTextChangedListener(this);

        setContentView(mLayout);
    }

    override fun onResume() {
        super.onResume()
        if (requestedOrientation != FORCED_ORIENTATION) {
            requestedOrientation = FORCED_ORIENTATION
        }
    }

    override fun onDestroy() {
        super.onDestroy()
    }

    private fun getSelfPackageInfo(): PackageInfo {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            packageManager.getPackageInfo(packageName, PackageManager.PackageInfoFlags.of(0))
        } else {
            @Suppress("DEPRECATION")
            packageManager.getPackageInfo(packageName, 0)
        }
    }

    private fun getAssetsSyncVersion(): String {
        val pkg = getSelfPackageInfo()
        val versionCode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            pkg.longVersionCode
        } else {
            @Suppress("DEPRECATION")
            pkg.versionCode.toLong()
        }
        return "$versionCode:${pkg.lastUpdateTime}"
    }

    private fun hasAssetSentinels(fdir: String): Boolean {
        return File("$fdir/resources").isDirectory &&
            File("$fdir/pipelines").isDirectory &&
            File("$fdir/satdump_cfg.json").isFile
    }

    private fun crc32OfStream(input: InputStream): Long {
        val crc = CRC32()
        val buffer = ByteArray(32 * 1024)
        input.use { stream ->
            while (true) {
                val read = stream.read(buffer)
                if (read <= 0) {
                    break
                }
                crc.update(buffer, 0, read)
            }
        }
        return crc.value
    }

    private fun getAssetLength(aman: AssetManager, rsrc: String): Long {
        return try {
            aman.openFd(rsrc).length
        } catch (_: IOException) {
            -1L
        }
    }

    private fun shouldCopyAsset(aman: AssetManager, local: File, rsrc: String): Boolean {
        if (!local.exists()) {
            return true
        }

        val assetLength = getAssetLength(aman, rsrc)
        if (assetLength >= 0 && local.length() != assetLength) {
            return true
        }

        if (assetLength < 0) {
            val assetCrc = crc32OfStream(aman.open(rsrc))
            val localCrc = FileInputStream(local).use { crc32OfStream(it) }
            if (assetCrc != localCrc) {
                return true
            }
        }

        return false
    }

    private fun copyAssetFile(aman: AssetManager, local: File, rsrc: String) {
        local.parentFile?.let { createIfDoesntExist(it.absolutePath) }
        aman.open(rsrc).use { input ->
            FileOutputStream(local).use { output ->
                val buffer = ByteArray(32 * 1024)
                while (true) {
                    val read = input.read(buffer)
                    if (read <= 0) {
                        break
                    }
                    output.write(buffer, 0, read)
                }
                output.flush()
            }
        }
    }

    private fun syncAssetEntry(aman: AssetManager, local: String, rsrc: String, stats: AssetSyncStats) {
        val children = aman.list(rsrc) ?: emptyArray()
        if (children.isNotEmpty()) {
            createIfDoesntExist(local)
            for (child in children) {
                syncAssetEntry(aman, "$local/$child", "$rsrc/$child", stats)
            }
            return
        }

        val localFile = File(local)
        stats.filesChecked += 1
        if (shouldCopyAsset(aman, localFile, rsrc)) {
            copyAssetFile(aman, localFile, rsrc)
            stats.filesCopied += 1
        }
    }

    public fun getAppDir(): String {
        val fdir = filesDir.absolutePath
        val aman = assets
        val startMs = SystemClock.elapsedRealtime()
        val stats = AssetSyncStats()
        val syncVersion = getAssetsSyncVersion()
        val prefs = getSharedPreferences(ASSET_SYNC_PREFS, Context.MODE_PRIVATE)
        val lastSyncedVersion = prefs.getString(ASSET_SYNC_KEY, null)
        val canSkip = lastSyncedVersion == syncVersion && hasAssetSentinels(fdir)

        if (!canSkip) {
            extractDir(aman, "$fdir/resources", "resources", stats)
            extractDir(aman, "$fdir/pipelines", "pipelines", stats)
            extractFile(aman, "$fdir/satdump_cfg.json", "satdump_cfg.json", stats)
            prefs.edit().putString(ASSET_SYNC_KEY, syncVersion).apply()
        }

        val elapsed = SystemClock.elapsedRealtime() - startMs
        Log.i(
            TAG,
            "Asset sync summary: skipped=$canSkip checked=${stats.filesChecked} copied=${stats.filesCopied} durationMs=$elapsed"
        )

        return fdir
    }

    public fun get_plugins_directory() : String {
        return getApplicationInfo().nativeLibraryDir;
    }

    public fun get_dpi() : Float {
        return getResources().getDisplayMetrics().density;
    }

    fun showSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        editText?.requestFocus()
        inputMethodManager.showSoftInput(editText, 0)
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(editText!!.windowToken, 0)
        editText?.clearFocus()
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // Not all Android keyboard trigger a KeyEvent
    // so I had to get around it somehow...
    // I'm not super proud of this and it has downsides,
    // but at least it works!
    // Hecking Android not having a simple function
    // to get Key events......... WHY!?
    override fun afterTextChanged(s : Editable) {
        if(!editText!!.getText().toString().startsWith(" "))
        {
            editText!!.setText(" ");
            editText!!.setSelection(1);
        }

        lastFiller = editText!!.getText().toString();
    }

    override fun beforeTextChanged(s : CharSequence, start: Int, count: Int, after: Int) {
    }

    override fun onTextChanged(s : CharSequence, start: Int, before: Int, count: Int) {
        if(editText!!.getText().toString() != lastFiller) {
            if(before < count) {
                var char2 = s.get(s.length - 1);
                unicodeCharacterQueue.offer(char2.code);
            } else if(before > count) {
                unicodeCharacterQueue.offer(8); // BackSpace
            }
        }
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun extractFile(aman: AssetManager, local: String, rsrc: String): Int {
        return extractFile(aman, local, rsrc, AssetSyncStats())
    }

    private fun extractFile(aman: AssetManager, local: String, rsrc: String, stats: AssetSyncStats): Int {
        syncAssetEntry(aman, local, rsrc, stats)
        return 0
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        return extractDir(aman, local, rsrc, AssetSyncStats())
    }

    private fun extractDir(aman: AssetManager, local: String, rsrc: String, stats: AssetSyncStats): Int {
        val checkedBefore = stats.filesChecked
        syncAssetEntry(aman, local, rsrc, stats)
        return stats.filesChecked - checkedBefore
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    // Handle selecting a file
    var select_file_result : String = "";
    public fun select_file() {
        var file_intent = Intent(Intent.ACTION_GET_CONTENT);
        file_intent.setType("*/*");
        file_intent.addCategory(Intent.CATEGORY_OPENABLE);
        val final_intent = Intent.createChooser(file_intent, "Выберите файл");
        startActivityForResult(final_intent, 1);
    }

    public fun select_file_get() : String {
        var tmp = select_file_result;
        select_file_result = "";
        return tmp;
    }

    // Handle selecting a directory
    var select_directory_result : String = "";
    public fun select_directory() {
        var file_intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        file_intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        file_intent.addCategory(Intent.CATEGORY_DEFAULT);
        val final_intent = Intent.createChooser(file_intent, "Выберите папку");
        startActivityForResult(final_intent, 2);
    }

    public fun select_directory_get() : String {
        var tmp = select_directory_result;
        select_directory_result = "";
        return tmp;
    }

    public fun openURL(url: String) {
        val browserIntent = Intent(Intent.ACTION_VIEW, Uri.parse(url));
        startActivity(browserIntent);
    }

    // Receive results of the above
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == 1) {
            if(resultCode == RESULT_OK)
                select_file_result = data.getFilePath(getApplicationContext());
            else if(resultCode == RESULT_CANCELED)
                select_file_result = "NO_PATH_SELECTED";
        }

        if (requestCode == 2) {
            if(resultCode == RESULT_OK)
                select_directory_result = data.getFilePathDir(getApplicationContext());
            else if(resultCode == RESULT_CANCELED)
                select_directory_result = "NO_PATH_SELECTED";
        }
    }
}
