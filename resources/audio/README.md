audio was converted with this command:

```
ffmpeg -i toyphone_part1.mp3 -ar 16000 -ac 1 -acodec pcm_s8 -f s8 toyphone_part1.raw
```
