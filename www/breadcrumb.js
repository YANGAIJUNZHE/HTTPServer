/**
 * 上程数据 - 面包屑导航
 * 每个页面声明 CURRENT_PAGE，脚本根据当前页所处的层级自动生成面包屑。
 */
(function() {
  // ========== 网站结构 ==========
  var SITE = {
    home: { label: '主页', url: 'index.html' },
    categories: [{
      label: '网络服务器目录页',
      url:   '项目案例-网络服务器.html',
      pages: [
        { label: '01. OSI七层模型',                url: '01-OSI七层模型.html' },
        { label: '02. Socket编程概念',              url: '02-Socket编程概念.html' },
        { label: '03. 预备知识',                    url: '03-预备知识.html' },
        { label: '04. 创建一个TCP服务器及客户端',    url: '04-创建一个TCP服务器及客户端.html' },
        { label: '05. 一次连接多次请求',            url: '05-一次连接多次请求.html' },
        { label: '06. 并发请求',                    url: '06-并发请求.html' },
        { label: '07. 发送指令',                    url: '07-发送指令.html' },
        { label: '08. 解析指令并返回',              url: '08-解析指令并返回.html' },
        { label: '09. 存储KV',                      url: '09-存储KV.html' },
        { label: '示例：echo服务器',                url: '示例-echo服务器.html' },
        { label: '示例：web服务器',                 url: '示例-web服务器.html' },
      ]
    }]
  };

  var cur = typeof CURRENT_PAGE !== 'undefined' ? CURRENT_PAGE : '';

  // ========== 确定当前页所属层级 ==========
  // level: 'home' | 'category' | 'page'
  var level, cat, page;

  if (cur === SITE.home.url) {
    level = 'home';
  } else {
    for (var i = 0; i < SITE.categories.length; i++) {
      var c = SITE.categories[i];
      if (cur === c.url) {
        level = 'category';
        cat = c;
        break;
      }
      for (var j = 0; j < c.pages.length; j++) {
        if (cur === c.pages[j].url) {
          level = 'page';
          cat = c;
          page = c.pages[j];
          break;
        }
      }
      if (cat) break;
    }
  }

  // 未匹配到任何页面，不渲染面包屑
  if (!level) return;

  var bc = document.getElementById('breadcrumb');
  if (!bc) return;

  // ========== 辅助：生成下拉菜单 HTML ==========
  function dropdownHTML(items) {
    var h = '<span class="breadcrumb-dropdown">';
    h += '<span class="breadcrumb-toggle">›</span>';
    h += '<ul class="breadcrumb-menu">';
    for (var k = 0; k < items.length; k++) {
      h += '<li><a href="' + items[k].url + '">' + items[k].label + '</a></li>';
    }
    h += '</ul></span>';
    return h;
  }

  // ========== 按层级渲染面包屑 HTML ==========
  var html = '';

  if (level === 'home') {
    // 主页：无链接、无下拉
    html = '<span class="breadcrumb-current">' + SITE.home.label + '</span>';

  } else if (level === 'category') {
    // 主页链接
    html += '<a href="' + SITE.home.url + '">' + SITE.home.label + '</a>';
    // 分类切换下拉
    html += dropdownHTML(SITE.categories);
    // 当前分类（真实链接）
    html += '<a href="' + cat.url + '">' + cat.label + '</a>';

  } else if (level === 'page') {
    // 主页链接
    html += '<a href="' + SITE.home.url + '">' + SITE.home.label + '</a>';
    // 分类切换下拉
    html += dropdownHTML(SITE.categories);
    // 当前分类链接
    html += '<a href="' + cat.url + '">' + cat.label + '</a>';
    // 同分类页面切换下拉
    html += dropdownHTML(cat.pages);
    // 当前页面链接
    html += '<a href="' + page.url + '">' + page.label + '</a>';
  }

  bc.innerHTML = html;

  // ========== 事件：点击委托 ==========
  bc.addEventListener('click', function(e) {
    var toggle = e.target.closest('.breadcrumb-toggle');
    if (!toggle) return;
    e.stopPropagation();
    var dd = toggle.closest('.breadcrumb-dropdown');
    var wasOpen = dd.classList.contains('open');
    // 关闭所有下拉
    var allOpen = bc.querySelectorAll('.breadcrumb-dropdown.open');
    for (var i = 0; i < allOpen.length; i++) allOpen[i].classList.remove('open');
    var allT = bc.querySelectorAll('.breadcrumb-toggle');
    for (var t = 0; t < allT.length; t++) allT[t].textContent = '›';
    // 打开当前
    if (!wasOpen) { dd.classList.add('open'); toggle.textContent = '▼'; }
  });

  // 点击外部关闭
  document.addEventListener('click', function(e) {
    if (!e.target.closest('#breadcrumb')) {
      var allOpen = bc.querySelectorAll('.breadcrumb-dropdown.open');
      for (var i = 0; i < allOpen.length; i++) allOpen[i].classList.remove('open');
      var allT = bc.querySelectorAll('.breadcrumb-toggle');
      for (var t = 0; t < allT.length; t++) allT[t].textContent = '›';
    }
  });
})();
