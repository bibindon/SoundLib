# SoundLib

DirectX9 / DirectSound を使って、`wav` の効果音、BGM、環境音を簡単に扱うためのライブラリです。

このリポジトリには次の2つが入っています。

- `SoundLib`
  DirectSound ベースの静的ライブラリ本体です。
- `simple-directx9`
  ライブラリの動作確認用サンプルです。

## 対応内容

- `wav` ファイルのみ対応
- 効果音の事前読み込み
- 効果音の同時再生
- BGM のループ再生
- BGM のフェードイン / フェードアウト
- 環境音のループ再生
- リスナー位置に応じた簡易 3D 表現
- 効果音 / 環境音ごとの再生 ID 管理

## 仕様の考え方

このライブラリでは音を3種類に分けています。

- 効果音
  反応速度を重視する音です。再生直前にファイルを読むと遅れるので、先に `LoadSoundEffect` で読み込みます。
- BGM
  常に1本だけ鳴る想定の音です。必要になった時点で読み込み、ループ再生します。
- 環境音
  川の音、雨音、森の音のような、場所に結びついた音です。必要になった時点で読み込みます。

## 使い方

### 1. 初期化

最初に一度だけ `Initialize` を呼びます。

```cpp
SoundLib::SoundLib::Initialize(hWnd);
```

`hWnd` にはウィンドウハンドルを渡します。

### 2. 毎フレーム更新

毎フレーム `Update` を呼びます。

```cpp
SoundLib::Vector3 listenerPosition { 0.0f, 0.0f, 0.0f };
SoundLib::Vector3 listenerFront { 0.0f, 0.0f, 1.0f };
SoundLib::Vector3 listenerTop { 0.0f, 1.0f, 0.0f };

SoundLib::SoundLib::Update(listenerPosition, listenerFront, listenerTop);
```

この更新で次の処理が進みます。

- フェードイン / フェードアウト
- リスナー移動に応じた左右パンと距離減衰
- 再生終了した効果音・環境音の後始末

### 3. 効果音の読み込みと再生

効果音は先に読み込みます。

```cpp
SoundLib::SoundLib::LoadSoundEffect(L"res\\arrowMiss.wav");
```

再生時は `PlaySoundEffect` を呼びます。

```cpp
int seId = SoundLib::SoundLib::PlaySoundEffect(L"res\\arrowMiss.wav", 80);
```

位置つきで鳴らす場合は座標を渡します。

```cpp
SoundLib::Vector3 position { 10.0f, 0.0f, 5.0f };
int seId = SoundLib::SoundLib::PlaySoundEffect(
    L"res\\attack01.wav",
    80,
    &position,
    SoundLib::EffectType::Radio);
```

停止したい場合は再生 ID を使います。

```cpp
SoundLib::SoundLib::StopSoundEffect(seId);
```

### 4. BGM の再生

```cpp
SoundLib::SoundLib::PlayBgm(L"res\\doukutsu.wav", 75);
```

再生開始位置を秒で指定することもできます。

```cpp
SoundLib::SoundLib::PlayBgm(L"res\\doukutsu.wav", 75, 10.0f);
```

音量変更:

```cpp
SoundLib::SoundLib::SetBgmVolume(60);
```

停止:

```cpp
SoundLib::SoundLib::StopBgm();
```

同じ BGM を再生中にもう一度 `PlayBgm` を呼んだ場合は無視されます。
別の BGM を指定した場合は、現在の BGM をフェードアウトしてから新しい BGM へ切り替わります。

### 5. 環境音の再生

```cpp
SoundLib::Vector3 envPos { -15.0f, 0.0f, 8.0f };
int envId = SoundLib::SoundLib::PlayEnvironmentSound(
    L"res\\ENV_forest.wav",
    70,
    &envPos,
    SoundLib::EffectType::Cave,
    true);
```

停止:

```cpp
SoundLib::SoundLib::StopEnvironmentSound(envId);
```

音量変更:

```cpp
SoundLib::SoundLib::SetEnvironmentSoundVolume(envId, 50);
```

## 内部の仕組み

### wav の扱い

ライブラリは `wav` ファイルの `fmt` チャンクと `data` チャンクを読み取り、`PCM` データをそのまま DirectSound のバッファへ書き込みます。

そのため、圧縮形式の音声ではなく、PCM の `wav` を前提にしています。

### DirectSound のバッファ

DirectSound では、実際に再生する単位は `secondary buffer` です。

このライブラリでは次の考え方を使っています。

- 効果音を1回鳴らす
  1本の secondary buffer を作って再生する
- 同じ効果音を同時に5回鳴らす
  5本の secondary buffer を作ってそれぞれ再生する

つまり、多重再生は「同じバッファを重ねる」のではなく、「再生インスタンスごとに別バッファを持つ」形です。

### 同時再生数

- 効果音: 全体で最大16個
- 環境音: 全体で最大16個

上限を超えた場合は例外になります。

### 簡易3D表現

このライブラリは、今回の実装では DirectSound3D に全面依存していません。

代わりに次の2つで空間感を表現しています。

- 音源とリスナーの距離に応じた音量減衰
- 音源が左右どちらにあるかに応じたパン

この方式にした理由は、ステレオの `wav` でも扱いやすくするためです。

### フェード

フェードは別スレッドではなく、`Update` が呼ばれるたびに少しずつ音量を変えて実現しています。

- BGM の再生開始時
  0 から目標音量までフェードイン
- BGM の停止時
  現在音量から 0 までフェードアウト
- 環境音の停止時
  現在音量から 0 までフェードアウト

効果音はフェードしません。

## サンプルプログラム

`simple-directx9` は `res` フォルダの音を自動で使います。

- ファイルサイズが小さいもの
  効果音として使用
- ファイルサイズが大きいもの
  BGM として使用
- ファイル名に `ENV` を含むもの
  環境音として使用

### キー操作

- `1`
  効果音を1回再生
- `2`
  効果音を5回同時再生
- `3`
  位置つき効果音を再生
- `B`
  BGM の再生 / 停止
- `N`
  次の BGM へ切り替え
- `E`
  左側の環境音の再生 / 停止
- `R`
  右側の環境音の再生 / 停止
- `+` / `-`
  BGM 音量変更
- 矢印キー
  リスナー移動

## 注意点

- `Initialize` を呼ぶ前に再生 API を使わないでください。
- 終了時には `Finalize` を呼んでください。
- 効果音は `LoadSoundEffect` 済みである必要があります。
- PCM 以外の `wav` は対応していません。
- 多重再生数の上限を超えると例外になります。

## ビルド

このリポジトリでは `Debug|x64` で動作確認しています。

サンプル実行ファイル:

- [simple-directx9.exe](/C:/Users/bibindon/source/repos/bibindon/SoundLib/x64/Debug/simple-directx9.exe)
