#requires -Version 7.0
# 从同一布局生成项目架构 SVG 与 PNG，图中仅陈述已实现组件和真实装配关系。
# 步骤：准备画布与字体 → 绘制主要连接和组件 → 标注执行边界 → 保存矢量及位图并释放绘图资源。
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '../docs'))
Add-Type -AssemblyName System.Drawing
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$width = 1540; $height = 1070
$bitmap = [Drawing.Bitmap]::new($width,$height)
$canvas = [Drawing.Graphics]::FromImage($bitmap)
$canvas.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$canvas.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$svg = [Text.StringBuilder]::new()
[void]$svg.AppendLine('<svg xmlns="http://www.w3.org/2000/svg" width="1540" height="1070" viewBox="0 0 1540 1070" role="img" aria-label="大肥鱼当前项目架构图">')
[void]$svg.AppendLine('<defs><marker id="arrow" markerUnits="userSpaceOnUse" markerWidth="12" markerHeight="12" refX="11" refY="6" orient="auto-start-reverse"><path d="M0 0L12 6L0 12Z" fill="#385974"/></marker></defs>')

# 转换统一的颜色表示，位图和 SVG 使用相同色值。
function Color([string]$Value) { return [Drawing.ColorTranslator]::FromHtml($Value) }

# 在两种输出中绘制同一文字，SVG 内容严格转义，不包含可执行脚本。
function Label([string]$Text,[int]$X,[int]$Y,[int]$Size=16,[string]$Ink='#243b53',[bool]$Bold=$false) {
    $style = if($Bold){[Drawing.FontStyle]::Bold}else{[Drawing.FontStyle]::Regular}
    $font = [Drawing.Font]::new('Microsoft YaHei UI',$Size,$style,[Drawing.GraphicsUnit]::Pixel)
    $brush = [Drawing.SolidBrush]::new((Color $Ink))
    try { $canvas.DrawString($Text,$font,$brush,[single]$X,[single]$Y) } finally { $font.Dispose(); $brush.Dispose() }
    $safe = [Security.SecurityElement]::Escape($Text); $baseline = $Y+$Size; $weight = if($Bold){'700'}else{'400'}
    [void]$svg.AppendLine("<text x='$X' y='$baseline' fill='$Ink' font-size='$Size' font-weight='$weight' font-family='Microsoft YaHei UI,Microsoft YaHei,Segoe UI,sans-serif'>$safe</text>")
}

# 依据固定坐标绘制折线及箭头，虚线表示 C ABI 或可选能力边界。
# 步骤：准备点列与笔 → 绘制连接和箭头 → 写入相同 SVG 折线 → 释放画笔。
function Arrow([object[]]$Points,[bool]$Dashed=$false,[bool]$Both=$false) {
    $line = [Drawing.PointF[]]::new($Points.Count)
    for($i=0;$i -lt $Points.Count;++$i){$line[$i]=[Drawing.PointF]::new($Points[$i][0],$Points[$i][1])}
    $pen = [Drawing.Pen]::new((Color '#385974'),2)
    $cap = [Drawing.Drawing2D.AdjustableArrowCap]::new(5,6,$true)
    try {
        if($Dashed){$pen.DashStyle=[Drawing.Drawing2D.DashStyle]::Dash}
        $pen.CustomEndCap=$cap; if($Both){$pen.CustomStartCap=$cap}
        $canvas.DrawLines($pen,$line)
        $encoded = ($Points|ForEach-Object{[string]$_[0]+','+[string]$_[1]}) -join ' '
        $dash = if($Dashed){"stroke-dasharray='6 5'"}else{''}; $start=if($Both){"marker-start='url(#arrow)'"}else{''}
        [void]$svg.AppendLine("<polyline points='$encoded' fill='none' stroke='#385974' stroke-width='2' $dash $start marker-end='url(#arrow)'/>")
    } finally {$pen.Dispose();$cap.Dispose()}
}

# 用一致的圆角框和文字层级表示组件，灰色虚框表示已实现但未默认接入问答的接口。
# 步骤：绘制组件底色和边界 → 写入 SVG 节点 → 绘制标题及逐行职责。
function Box($Node) {
    $x=$Node.x; $y=$Node.y; $w=$Node.w; $h=$Node.h; $r=14
    $path=[Drawing.Drawing2D.GraphicsPath]::new(); $pen=[Drawing.Pen]::new((Color $Node.stroke),1.6)
    $brush=[Drawing.SolidBrush]::new((Color $Node.fill))
    try {
        $path.AddArc($x,$y,$r*2,$r*2,180,90); $path.AddArc($x+$w-$r*2,$y,$r*2,$r*2,270,90)
        $path.AddArc($x+$w-$r*2,$y+$h-$r*2,$r*2,$r*2,0,90); $path.AddArc($x,$y+$h-$r*2,$r*2,$r*2,90,90);$path.CloseFigure()
        if($Node.dashed){$pen.DashStyle=[Drawing.Drawing2D.DashStyle]::Dash}
        $canvas.FillPath($brush,$path);$canvas.DrawPath($pen,$path)
    } finally {$path.Dispose();$pen.Dispose();$brush.Dispose()}
    $dash=if($Node.dashed){"stroke-dasharray='7 5'"}else{''}
    [void]$svg.AppendLine("<rect x='$x' y='$y' width='$w' height='$h' rx='14' fill='$($Node.fill)' stroke='$($Node.stroke)' stroke-width='1.6' $dash/>")
    Label $Node.title ($x+17) ($y+13) 18 '#17324d' $true
    $row=$y+44; foreach($line in $Node.lines){Label $line ($x+17) $row 15; $row+=24}
}

try {
    $canvas.Clear((Color '#f7f9fc'));[void]$svg.AppendLine('<rect width="1540" height="1070" fill="#f7f9fc"/>')
    Label 'DaFeiYu  当前项目架构' 60 24 28 '#17324d' $true
    Label '审查后实现 · Windows x64 / C++20 / C ABI' 60 66 16 '#52677b'
    Label '用户 / 终端输入' 655 132 19 '#17324d' $true
    Arrow @(@(765,163),@(765,195))
    Arrow @(@(370,255),@(440,255)) $false $true; Label '输入' 384 230 13
    Arrow @(@(575,315),@(575,390)); Label '问答' 586 345 13
    Arrow @(@(440,285),@(405,285),@(405,350),@(215,350),@(215,390)); Label '本地管理命令' 235 326 13
    Arrow @(@(710,450),@(790,450)); Label '提交 / 结果' 716 425 12
    Arrow @(@(1090,450),@(1170,450)); Label '供应商协议' 1095 425 12
    Arrow @(@(1320,510),@(1320,610)); Label 'HTTPS' 1330 553 13
    Arrow @(@(215,510),@(215,610)); Label '实例与宿主能力' 228 552 13
    Arrow @(@(575,510),@(575,610)); Label '不可变会话事实' 586 552 13
    Arrow @(@(370,684),@(440,684)) $false $true; Label '读取/数据' 377 660 12
    Arrow @(@(710,684),@(790,684)); Label '类型/操作' 718 660 12
    Arrow @(@(215,760),@(215,810)) $true; Label 'SDK · C ABI v2' 226 777 13
    Arrow @(@(370,735),@(405,735),@(405,790),@(575,790),@(575,810)); Label '代码持有' 436 764 13
    Arrow @(@(440,875),@(370,875)); Label '动态加载' 380 851 12
    Arrow @(@(940,810),@(940,760)) $true
    Arrow @(@(60,450),@(28,450),@(28,975),@(1320,975),@(1320,940)); Label '安全暂存 / 原子发布 / 独立用户设置' 760 978 14 '#52677b'

    $nodes=@(
        @{x=60;y=195;w=310;h=120;title='Platform · LineInput';lines=@('专用读取线程 / 有界行队列','UTF-16 → UTF-8','取消读取后确认实际回收');fill='#fff4d8';stroke='#bf9145'},
        @{x=440;y=195;w=650;h=120;title='App · 交互宿主（主线程）';lines=@('入口与路径 / ChatController / plugin_commands','交替推进模型、插件和输入；本地命令不发送给模型');fill='#e8f1fb';stroke='#668bb3'},
        @{x=60;y=390;w=310;h=120;title='Plugins · PluginManager';lines=@('目录 / 安装 / 移除 / 实例','可选依赖绑定与去重停止计划','失败保留，明确重试');fill='#e8f1fb';stroke='#668bb3'},
        @{x=440;y=390;w=270;h=120;title='Agent · Session';lines=@('单轮问答与上下文','完整成功后成对提交历史','取消与实际退出分开');fill='#e8f1fb';stroke='#668bb3'},
        @{x=790;y=390;w=300;h=120;title='Model · AsyncClient';lines=@('有界后台完整文本调用','内置 DeepSeek / Qwen 适配','结果仅由宿主线程结算');fill='#fff4d8';stroke='#bf9145'},
        @{x=1170;y=390;w=300;h=120;title='Platform · WinHTTP';lines=@('系统 TLS / 分阶段超时','不跟随重定向','完整响应大小限制');fill='#fff4d8';stroke='#bf9145'},
        @{x=60;y=610;w=310;h=150;title='PluginInstance / HostServices';lines=@('创建 / 启动 / 停止 / 销毁','宿主线程绑定与能力表','读取 / 通知 / 引用适配','忙碌拒绝，停止失败保留');fill='#e8f1fb';stroke='#668bb3'},
        @{x=440;y=610;w=270;h=150;title='Events · 不可变事件流';lines=@('EventStore / BroadcastChannel','独立订阅 / 按授权组消费','有界接纳与读请求','发布时不内联调用插件');fill='#e8f1fb';stroke='#668bb3'},
        @{x=790;y=610;w=300;h=150;title='Runtime · 共用基础';lines=@('类型 / 引用 / 操作 / 预算','CompletionNotifications','OwnerScheduler / 公平调度','ResourceOwner / 生命周期');fill='#e8f1fb';stroke='#668bb3'},
        @{x=1170;y=610;w=300;h=100;title='外部模型服务';lines=@('DeepSeek / Qwen','插件为空也可完成问答');fill='#fff4d8';stroke='#bf9145'},
        @{x=60;y=810;w=310;h=130;title='可选可信原生 DLL';lines=@('observer 0.7.0 示例','同进程 / 宿主线程回调','独立安装、加载和卸载');fill='#e3f4ed';stroke='#5d9982'},
        @{x=440;y=810;w=270;h=130;title='Platform · DLL 代码寿命';lines=@('CodeModule / CodeLease','CodeCall 活动保护','DLL 目录 + 系统目录加载');fill='#e8f1fb';stroke='#668bb3'},
        @{x=790;y=810;w=300;h=130;title='已实现的可选基础接口';lines=@('Processing / Serial / 等待图','ResultGroup / ControlGate / Retry','默认问答未启用这些业务流程');fill='#eef1f5';stroke='#8895a4';dashed=$true},
        @{x=1170;y=810;w=300;h=130;title='磁盘与用户配置';lines=@('plugins/<id>：包与默认配置','用户目录：独立 settings.json','移除包保留用户设置');fill='#eef1f5';stroke='#8895a4'}
    )
    foreach($node in $nodes){Box $node}
    Label '蓝：宿主状态与调度   黄：后台 I/O / 外部服务   绿：可信 DLL 边界   灰虚框：非默认业务接口' 60 1018 15 '#52677b'
    Label '按主要运行路径绘制，共用运行时的部分依赖边省略；本体不静态链接插件实现。' 60 1042 14 '#52677b'
    [void]$svg.AppendLine('</svg>')
    [IO.File]::WriteAllText((Join-Path $output 'architecture.svg'),$svg.ToString(),[Text.UTF8Encoding]::new($false))
    $bitmap.Save((Join-Path $output 'architecture.png'),[Drawing.Imaging.ImageFormat]::Png)
} finally {$canvas.Dispose();$bitmap.Dispose()}
