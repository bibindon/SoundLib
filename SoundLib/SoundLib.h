//-----------------------------------------------------------
// 効果音とBGMを簡単に扱えるようにするためのライブラリ
// 
// ・wavファイルだけ対応
// ・インスタンスを作成する必要はない。
//   ・最初に静的なInitialize関数を呼べば、以降は静的関数だけで操作できる。
//   ・終了時に静的なFinalize関数をユーザーは必ず呼ばないといけない。
// ・BGM、効果音、環境音は別々に考える必要がある。
// ・Update関数でリスナーの位置・向きを設定できる。
//
// ・BGM
//   ・Play関数を実行したら読み込んで再生
//     ・Play関数は引数で音量を設定できる。0～100の101段階
//     ・Play関数は引数で再生開始位置を指定できる。引数の単位は秒。引数ナシなら最初から再生。
//   ・Stop関数で停止する
//     ・Stop関数で停止した後、Play関数を実行したら最初からBGMが再生される。
//   ・SetVolume関数で音量を設定できる。0～100の101段階
//   ・Play関数とStop関数はフェードインとフェードアウトが自動で行われる。
//     SetVolume関数で音量を30と設定されていたら、0～30の間でフェードイン、フェードアウトが行われる。
//   ・音源の位置などは考慮する必要がない
//   ・BGM再生中にPlay関数を実行した場合、同じBGMなら無視。
//     違うBGMだったら、フェードアウトしてから新しいBGMをフェードインしながら再生
//     二つのBGMを同時に再生することはない。
//   ・最後まで再生したら最初から再生しなおす。
// 
// ・効果音
//   ・Load関数で効果音を読み込む
//   ・Play関数を実行したら再生。読み込まれていなかったら例外が発生する。
//     ・Play関数は引数で音量を設定できる。0～100の101段階
//     ・Play関数の引数で音源の3D座標を指定できる。座標が指定された場合、効果音の大きさやヘッドフォンの左右で差が生じる。
//     ・Play関数の引数でエフェクトを設定できる。
//     ・戻り値としてIDを受け取る。失敗時は例外を送出する。
//   ・Stop関数で停止する
//     ・引数にIDを指定する。
//     ・Stop関数で停止した後、Play関数を実行したら最初から効果音が再生される。
//   ・SetVolume関数は無し。
//   ・効果音ではフェードインとフェードアウトは必要ない。フェードイン・フェードアウトをしてはいけない。
//   ・効果音は複数同時に再生できる。ただし、効果音全体で同時再生数は16個まで。上限を超えたら再生されない。
// 
// ・環境音
//   ・Load関数は不要
//   ・Play関数を実行したら読み込んで再生。
//     ・Play関数は引数で音量を設定できる。0～100の101段階
//     ・Play関数の引数で音源の3D座標を指定できる。座標が指定された場合、環境音の大きさやヘッドフォンの左右で差が生じる。
//     ・Play関数の引数でエフェクトを設定できる。
//     ・Play関数の引数でループ再生するかしないかを設定できる。デフォルトはループ再生あり。
//     ・戻り値としてIDを受け取る。失敗時は例外を送出する。
//   ・Stop関数で停止する
//     ・引数にIDを指定する。
//     ・Stop関数で停止した後、Play関数を実行したら最初から環境音が再生される。
//   ・SetVolume関数で音量を設定できる。0～100の101段階
//   ・環境音ではフェードインとフェードアウトは自動で行われる。
//   ・環境音は複数同時に再生できる。ただし、環境音全体で同時再生数は16個まで。上限を超えたら再生されない。
// 
//-----------------------------------------------------------

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>

namespace SoundLib
{

struct Vector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class EffectType
{
    // 加工なし。
    None,
    // 少しこもった音に寄せる。
    Muffle,
    // 無線っぽい軽い加工に寄せる。
    Radio,
    // 反響感のある少し鈍い音に寄せる。
    Cave
};

class SoundLib
{
public:
    // DirectSound 全体の初期化を行う。
    // 最初に一度だけ呼ぶ想定。
    static void Initialize(HWND windowHandle);

    // 内部で保持しているバッファやキャッシュを解放する。
    // アプリ終了時に一度だけ呼ぶ想定。
    static void Finalize();

    // リスナーの位置と向きを更新する。
    // 3D 座標つきの効果音・環境音はこの情報を使って聞こえ方を変える。
    static void Update(const Vector3& listenerPosition,
                       const Vector3& listenerFront,
                       const Vector3& listenerTop);

    // 効果音を事前読み込みする。
    // 効果音は即時反応が欲しいので、Play 前にロードしておく前提。
    static void LoadSoundEffect(const std::wstring& filePath);

    // 読み込み済みの効果音を再生する。
    // 戻り値は停止に使う再生ID。
    // sourcePosition を指定した場合は、リスナーとの相対位置で音量と左右差を付ける。
    static int PlaySoundEffect(const std::wstring& filePath,
                               int volume = 100,
                               const Vector3* sourcePosition = nullptr,
                               EffectType effectType = EffectType::None);

    // 指定IDの効果音を停止する。
    static void StopSoundEffect(int id);

    // BGM を再生する。
    // すでに別の BGM が鳴っている場合は、現在の BGM をフェードアウトしてから切り替える。
    static void PlayBgm(const std::wstring& filePath,
                        int volume = 100,
                        float startSeconds = 0.0f);

    // 現在の BGM をフェードアウト付きで停止する。
    static void StopBgm();

    // BGM の目標音量を変更する。
    // フェード中ならその途中値から新しい音量へ向かう。
    static void SetBgmVolume(int volume);

    // 環境音を再生する。
    // 戻り値は停止や音量変更に使う再生ID。
    // 効果音と違って事前 Load は不要で、必要になった時点で内部ロードする。
    static int PlayEnvironmentSound(const std::wstring& filePath,
                                    int volume = 100,
                                    const Vector3* sourcePosition = nullptr,
                                    EffectType effectType = EffectType::None,
                                    bool loop = true);

    // 指定IDの環境音をフェードアウト付きで停止する。
    static void StopEnvironmentSound(int id);

    // 指定IDの環境音の音量を変更する。
    static void SetEnvironmentSoundVolume(int id, int volume);
};

}
