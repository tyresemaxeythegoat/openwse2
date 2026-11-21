package org.wse2.android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import org.wse2.android.launcher.R4StartupRunner

// Keeps the R4 startup run going while the launcher is in the background, the screen is off, or
// the app was swiped away from Recents (docs/RECOMP_PLAN.md "Phone run"). It runs in the launcher's
// own process, where R4StartupRunner holds the binding to the ":recomp" worker: a foreground
// service keeps this process (and, through the binding, the worker) alive, and a partial wake lock
// keeps the CPU running. The ongoing notification shows the progress and has a Stop button; when
// the run ends, R4StartupRunner posts the result notification, which opens the report. It is sticky
// while a run goes on, so a killed launcher process comes back and resumes the run.
class R4ForegroundService : Service() {

    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = progressNotification(this, "Starting")
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                startForeground(PROGRESS_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
            } else {
                startForeground(PROGRESS_ID, notification)
            }
        } catch (_: Exception) {
            // Android 12+ may refuse a sticky restart from the background; the next app start resumes.
            stopSelf()
            return START_NOT_STICKY
        }
        val runner = R4StartupRunner.get(application)
        if (intent?.action == ACTION_STOP) runner.stop()
        // A sticky restart after Android killed this process mid-run: start the run again.
        if (intent == null) runner.resumeIfInterrupted()
        // Stopped, or the run ended before the service started (a setup error, a stale Stop).
        if (!runner.isRunning()) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (wakeLock == null) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "wse2:r4-startup-run").apply {
                setReferenceCounted(false)
                acquire(WAKE_LOCK_LIMIT_MS)
            }
        }
        // Sticky: if Android kills this process mid-run it recreates the service, which resumes.
        return START_STICKY
    }

    override fun onDestroy() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL_PROGRESS = "r4_progress"
        private const val CHANNEL_RESULT = "r4_result"
        private const val PROGRESS_ID = 4001
        private const val RESULT_ID = 4002
        private const val ACTION_STOP = "org.wse2.android.action.R4_STOP"
        // A run translates for minutes and then runs 1,000 frames; never hold the CPU past this.
        private const val WAKE_LOCK_LIMIT_MS = 3L * 60 * 60 * 1000

        fun start(context: Context) {
            ensureChannels(context)
            context.startForegroundService(Intent(context, R4ForegroundService::class.java))
        }

        // Updates the ongoing notification (only while the service is in the foreground).
        fun showProgress(context: Context, status: String) {
            manager(context).notify(PROGRESS_ID, progressNotification(context, status))
        }

        // Ends the foreground service and posts the result, which opens the launcher's report.
        fun finish(context: Context, headline: String) {
            context.stopService(Intent(context, R4ForegroundService::class.java))
            manager(context).cancel(PROGRESS_ID)
            ensureChannels(context)
            val notification = Notification.Builder(context, CHANNEL_RESULT)
                .setSmallIcon(android.R.drawable.stat_notify_more)
                .setContentTitle("R4 startup run finished")
                .setContentText(headline.lineSequence().first())
                .setStyle(Notification.BigTextStyle().bigText(headline))
                .setContentIntent(openReport(context))
                .setAutoCancel(true)
                .build()
            manager(context).notify(RESULT_ID, notification)
        }

        fun clearResult(context: Context) = manager(context).cancel(RESULT_ID)

        private fun progressNotification(context: Context, status: String): Notification {
            ensureChannels(context)
            val stop = PendingIntent.getService(
                context,
                1,
                Intent(context, R4ForegroundService::class.java).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
            val open = PendingIntent.getActivity(
                context,
                2,
                Intent(context, WseLauncherActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
            return Notification.Builder(context, CHANNEL_PROGRESS)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("R4 startup run")
                .setContentText(status.ifEmpty { "Running" })
                .setStyle(Notification.BigTextStyle().bigText(status.ifEmpty { "Running" }))
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(open)
                .addAction(Notification.Action.Builder(null, "Stop", stop).build())
                .build()
        }

        private fun openReport(context: Context): PendingIntent = PendingIntent.getActivity(
            context,
            3,
            Intent(context, WseLauncherActivity::class.java)
                .putExtra(WseLauncherActivity.EXTRA_SHOW_R4_REPORT, true)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        private fun ensureChannels(context: Context) {
            val nm = manager(context)
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_PROGRESS, "R4 startup run progress", NotificationManager.IMPORTANCE_LOW)
            )
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_RESULT, "R4 startup run result", NotificationManager.IMPORTANCE_DEFAULT)
            )
        }

        private fun manager(context: Context) =
            context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    }
}
