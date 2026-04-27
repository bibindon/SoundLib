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
    None,
    Muffle,
    Radio,
    Cave
};

class SoundLib
{
public:
    static void Initialize(HWND windowHandle);
    static void Finalize();
    static void Update(const Vector3& listenerPosition,
                       const Vector3& listenerFront,
                       const Vector3& listenerTop);

    static void LoadSoundEffect(const std::wstring& filePath);
    static int PlaySoundEffect(const std::wstring& filePath,
                               int volume = 100,
                               const Vector3* sourcePosition = nullptr,
                               EffectType effectType = EffectType::None);
    static void StopSoundEffect(int id);

    static void PlayBgm(const std::wstring& filePath,
                        int volume = 100,
                        float startSeconds = 0.0f);
    static void StopBgm();
    static void SetBgmVolume(int volume);

    static int PlayEnvironmentSound(const std::wstring& filePath,
                                    int volume = 100,
                                    const Vector3* sourcePosition = nullptr,
                                    EffectType effectType = EffectType::None,
                                    bool loop = true);
    static void StopEnvironmentSound(int id);
    static void SetEnvironmentSoundVolume(int id, int volume);
};

}
