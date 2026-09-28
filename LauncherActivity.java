package com.example.sbageode;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.widget.Button;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;

public class LauncherActivity extends AppCompatActivity {

    // Загружаем наше C++ ядро, которое отключает лицензию Google Play
    static {
        System.loadLibrary("sba_geode_core");
    }

    public native void injectAndBypass(String package_name);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // Создаем простую кнопку запуска (в Geode UI гораздо красивее, но начнем с базы)
        Button launchButton = new Button(this);
        launchButton.setText("ЗАПУСТИТЬ SUPER BEAR ADVENTURE (GEODE)");
        setContentView(launchButton);

        launchButton.setOnClickListener(v -> {
            String gamePackage = "com.EarthkwakGames.SuperBearAdventure";
            
            if (isGameInstalled(gamePackage)) {
                Toast.makeText(this, "Запуск игры и обход лицензии...", Toast.LENGTH_SHORT).show();
                
                // 1. Активируем C++ хук лицензии
                injectAndBypass(gamePackage);
                
                // 2. Открываем саму игру
                Intent launchIntent = getPackageManager().getLaunchIntentForPackage(gamePackage);
                if (launchIntent != null) {
                    startActivity(launchIntent);
                }
            } else {
                Toast.makeText(this, "Игра Super Bear Adventure не установлена!", Toast.LENGTH_LONG).show();
            }
        });
    }

    private boolean isGameInstalled(String packageName) {
        try {
            getPackageManager().getPackageInfo(packageName, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }
}
