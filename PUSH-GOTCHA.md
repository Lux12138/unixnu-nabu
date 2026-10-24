# Push 卡住的根因与正确做法

## 症状
`git push origin main` 反复挂住，工具超时（400 秒）仍未返回；而同环境下
`curl https://api.github.com` 只要 0.23 秒、`gh repo create` / `gh repo fork` 秒回。

## 误判
一度以为被墙或代理干扰。实测排除：

| 检查 | 结果 |
|---|---|
| DNS | `20.205.243.166`，GitHub 真实 IP，无劫持 |
| HTTPS GET | HTTP 200，0.23 s |
| 证书 | `Sectigo` 签发，`Verify return code: 0 (ok)` —— 无中间人 |
| 代理环境变量 | 全部为空，git 也未配 proxy |
| 边缘节点 | `x-github-edge-region: southeastasia` —— 上游代理加速生效，链路通畅 |

**结论：不是网络问题，是调用方式问题。**

## 真正原因
1. **`for` 重试循环掩盖了真实状态**：`git push -q origin main` 在循环里失败后静默重试，
   整体耗时超过工具超时，看起来像"卡住"。
2. **trace 模式会提前退出**：`GIT_TRACE=1 GIT_CURL_VERBOSE=1 git push` 配 `timeout 20`，
   返回 `exit=0` 但远端 HEAD 未变 —— 它只完成了 `info/refs` 协商就被超时切断，
   **pack 上传没发生**。据此误判为"推送成功"。
3. 缺少推送后的**远端校验**，所以错误状态一直延续。

## 正确做法（三步，缺一不可）
1. **单次调用 + 硬超时**，不用重试循环：
   ```bash
   timeout 30 git push origin main 2>&1 | tail -5
   echo "exit=$?"
   ```
2. **立刻查远端 HEAD**：
   ```bash
   git ls-remote origin refs/heads/main | head -1
   ```
3. **用远端内容校验关键改动确实到位**，而不是只看本地：
   ```bash
   git fetch -q origin main
   git show origin/main:scripts/<file>.py | grep -c '<关键字符串>'
   ```

判定成功的唯一标准：**远端 `git show` 能查到改动**。本地 HEAD 领先不算数。
