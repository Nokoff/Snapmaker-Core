/*----My Files: a board of the home page listing a local folder. The app does all file work
      (MyFilesLibrary.cpp); this only draws what it is sent and asks for actions.----*/
var MyFiles = (function () {
	var SORT_KEY = 'myfiles_sort';
	var SORT_MODES = ['date_desc', 'date_asc', 'name_asc', 'name_desc'];
	var PLACEHOLDER = 'img/d.png';
	var isMac = /Mac|iPhone|iPad/i.test(navigator.platform || navigator.userAgent || '');

	var root = '';        // the mapped folder
	var subfolder = '';   // the one being browsed, relative to it
	var currentFolder = '';
	var recursive = false;
	var files = [];
	var images = {};      // preview by path, so re-sorting keeps them
	var selected = [];
	var anchor = '';      // where a Shift+click range starts
	var menuPath = '';
	var sortMode = 'date_desc';
	var progressTimer = null;

	function escapeHtml(text) {
		return String(text === null || text === undefined ? '' : text)
			.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
			.replace(/"/g, '&quot;').replace(/'/g, '&#39;');
	}

	function send(command, data) {
		var message = { sequence_id: Math.round(new Date() / 1000), command: command };
		if (data)
			message.data = data;
		SendWXMessage(JSON.stringify(message));
	}

	function isActive() {
		return $('#MyFilesBoard').is(':visible');
	}

	function refresh() {
		send('get_my_files');
	}

	function openFolder(rel) {
		send('myfiles_open_folder', { path: rel || '' });
	}

	function goUp() {
		if (!subfolder)
			return;
		var cut = subfolder.lastIndexOf('/');
		openFolder(cut > 0 ? subfolder.substring(0, cut) : '');
	}

	/*----Selection: files only, folders are never deleted or moved from here----*/
	function fileItems() {
		return $('#MyFilesList .MyFileItem').not('.MyFolderItem');
	}

	function showSelection() {
		fileItems().each(function () {
			$(this).toggleClass('Selected', selected.indexOf($(this).attr('fpath')) >= 0);
		});
		$('#MyFilesSelectedCount').text(T('myfiles_selected', 'Selected: %s', selected.length));
		$('#MyFilesSelectionBar').css('display', selected.length ? 'flex' : 'none');
	}

	function clearSelection() {
		selected = [];
		anchor = '';
		showSelection();
	}

	function select(path, toggle, range) {
		if (toggle) {
			var at = selected.indexOf(path);
			if (at >= 0)
				selected.splice(at, 1);
			else
				selected.push(path);
			anchor = path;
		} else if (range) {
			var paths = fileItems().map(function () { return $(this).attr('fpath'); }).get();
			var from = paths.indexOf(anchor || path);
			var to = paths.indexOf(path);
			if (from < 0)
				from = to;
			selected = paths.slice(Math.min(from, to), Math.max(from, to) + 1);
		} else {
			selected = [path];
			anchor = path;
		}
		showSelection();
	}

	function actionPaths(command) {
		hideMenu();
		if (selected.length)
			send(command, { paths: selected.slice() });
	}

	/*----Sorting is done here, as asking the app again would restart the previews----*/
	function sortDate(item) {
		var time = String(item.time || '');
		return /^\d{4}-\d{2}-\d{2}/.test(time) ? time : '';  // anything else says the date is unknown
	}

	function sorted(list) {
		var byName = sortMode.indexOf('name') === 0;
		var direction = /_desc$/.test(sortMode) ? -1 : 1;

		return list.slice().sort(function (a, b) {
			if (!!a.is_dir !== !!b.is_dir)
				return a.is_dir ? -1 : 1;  // folders lead, whatever the sort

			var diff = 0;
			if (byName) {
				diff = String(a.project_name || '').localeCompare(String(b.project_name || ''), undefined,
					{ numeric: true, sensitivity: 'base' });  // part2 before part10
			} else {
				var ta = sortDate(a), tb = sortDate(b);
				if (!ta !== !tb)
					return ta ? -1 : 1;
				diff = ta < tb ? -1 : (ta > tb ? 1 : 0);
			}
			return diff ? diff * direction : String(a.path || '').localeCompare(String(b.path || ''));
		});
	}

	/*----Drawing----*/
	function folderName(path) {
		var trimmed = String(path).replace(/[\\\/]+$/, '');
		return trimmed.substring(Math.max(trimmed.lastIndexOf('/'), trimmed.lastIndexOf('\\')) + 1) || trimmed;
	}

	function renderBreadcrumb() {
		var parts = subfolder ? subfolder.split('/') : [];
		var html = parts.length ?
			'<span class="MyFilesCrumb" frel="">' + escapeHtml(folderName(root)) + '</span>' : '';
		var rel = '';
		parts.forEach(function (part, i) {
			rel = rel ? rel + '/' + part : part;
			html += '<span class="MyFilesCrumbSep">/</span>' + (i === parts.length - 1 ?
				'<span class="MyFilesCrumbCurrent">' + escapeHtml(part) + '</span>' :
				'<span class="MyFilesCrumb" frel="' + escapeHtml(rel) + '">' + escapeHtml(part) + '</span>');
		});
		$('#MyFilesBreadcrumb').html(html).css('display', parts.length ? 'flex' : 'none');
		$('#MyFilesUpBtn').toggle(!!parts.length);
	}

	function tileHtml(item) {
		var path = escapeHtml(item.path);
		var name = '<div class="FileName">' + escapeHtml(item.project_name) + '</div>';

		if (item.is_dir) {
			var preview = images[item.preview_path];
			return '<div class="MyFileItem MyFolderItem" title="' + path + '" fpath="' + path + '" frel="' + escapeHtml(item.rel) +
				'" fpreview="' + escapeHtml(item.preview_path) + '">' +
				'<div class="FileImg"><div class="MyFolderGlyph"></div>' +
				(preview ? '<img class="MyFolderPreview" src="' + escapeHtml(preview) + '" alt="" />' : '') + '</div>' +
				name +
				'<div class="FileDate">' + escapeHtml(item.summary) + '</div></div>';
		}

		return '<div class="MyFileItem" title="' + path + '" fpath="' + path + '">' +
			'<div class="MyFileCheck"></div>' +
			'<div class="FileImg"><img class="MyFilePreview" src="' + escapeHtml(images[item.path] || PLACEHOLDER) + '" alt="" /></div>' +
			name +
			(item.rel_dir ? '<div class="MyFileFrom">' + escapeHtml(item.rel_dir) + '</div>' : '') +
			'<div class="FileDate">' + escapeHtml(item.time) + '</div></div>';
	}

	function renderList() {
		$('#MyFilesList').html(sorted(files).map(tileHtml).join(''));
		showSelection();
	}

	function renderHeader() {
		var mapped = !!root;
		var any = mapped && files.length > 0;

		$('#MyFilesPath').text(currentFolder).attr('title', currentFolder);
		renderBreadcrumb();

		$('#MyFilesNoFolder').css('display', mapped ? 'none' : 'flex');
		$('#MyFilesChangeBtn, #MyFilesRefreshBtn').toggle(mapped);
		// Stays on an empty folder, as that is how the files further down are found
		$('#MyFilesRecursiveBlock').css('display', mapped ? 'flex' : 'none');
		$('#MyFilesSortBlock').css('display', any ? 'flex' : 'none');
		$('#MyFilesHint, #MyFilesList').css('display', '').toggleClass('MyFilesShown', any);
		$('#MyFilesEmpty')
			.text(recursive ? T('myfiles_empty_recursive', 'No supported files in this folder or below it.')
			                : T('myfiles_empty', 'No supported files found in this folder.'))
			.toggle(mapped && !any);
	}

	function showList(payload) {
		root = payload.folder || '';
		subfolder = root ? (payload.subfolder || '') : '';
		currentFolder = payload.current_folder || root;
		recursive = !!payload.recursive;
		files = payload.response || [];

		clearSelection();
		hideMenu();
		$('#MyFilesRecursive').prop('checked', recursive);
		$('#MyFilesTruncated').toggle(!!payload.truncated);
		clearTimeout(progressTimer);
		$('#MyFilesProgress').hide();  // announced again if there is work left
		renderHeader();
		renderList();
	}

	function showProgress(done, total) {
		clearTimeout(progressTimer);
		if (!(total > 0)) {
			$('#MyFilesProgress').hide();
			return;
		}
		$('#MyFilesProgressCount').text(done + ' / ' + total);
		$('#MyFilesProgressBar').css('width', Math.min(100, Math.round(done / total * 100)) + '%');
		if (done >= total)
			progressTimer = setTimeout(function () { $('#MyFilesProgress').hide(); }, 300);
		else
			$('#MyFilesProgress').show();
	}

	function showPreview(path, image) {
		if (!path || !image)
			return;
		images[path] = image;
		$('#MyFilesList .MyFileItem').each(function () {
			var item = $(this);
			if (!item.hasClass('MyFolderItem')) {
				if (item.attr('fpath') === path)
					item.find('.MyFilePreview').attr('src', image);
			} else if (item.attr('fpreview') === path) {
				if (!item.find('.MyFolderPreview').length)
					item.find('.FileImg').append('<img class="MyFolderPreview" alt="" />');
				item.find('.MyFolderPreview').attr('src', image);
			}
		});
	}

	/*----Context menu----*/
	function hideMenu() {
		$('#MyFilesMenu').hide();
	}

	function showMenu(x, y, isFolder) {
		$('#MyFilesMenuDelete, #MyFilesMenuMove').toggle(!isFolder);
		var menu = $('#MyFilesMenu').show();
		menu.css({
			left: Math.max(0, Math.min(x, $(window).width() - menu.outerWidth() - 8)) + 'px',
			top: Math.max(0, Math.min(y, $(window).height() - menu.outerHeight() - 8)) + 'px'
		});
	}

	function onKey(e) {
		if (!isActive() || e.ctrlKey || e.metaKey)
			return;
		if (e.target && /^(SELECT|TEXTAREA)$/.test(e.target.tagName))
			return;

		// A Mac's Delete key sends Backspace. Elsewhere Backspace only ever goes up, as the hint says.
		var deletes = e.keyCode === 46 || (isMac && e.keyCode === 8 && selected.length > 0);
		if (e.keyCode === 27) {
			hideMenu();
			clearSelection();
		} else if (deletes) {
			e.preventDefault();
			actionPaths('myfiles_delete_files');
		} else if (e.keyCode === 8 || (e.keyCode === 37 && e.altKey)) {
			e.preventDefault();
			goUp();
		}
	}

	function bind() {
		$('#MyFilesList')
			.on('click', '.MyFileItem', function (e) {
				if ($(this).hasClass('MyFolderItem'))
					clearSelection();
				else
					select($(this).attr('fpath'), e.ctrlKey || e.metaKey, e.shiftKey);
			})
			.on('dblclick', '.MyFileItem', function (e) {
				if (e.ctrlKey || e.metaKey || e.shiftKey)
					return;  // those are selection gestures
				if ($(this).hasClass('MyFolderItem'))
					openFolder($(this).attr('frel'));
				else
					send('myfiles_open_file', { path: $(this).attr('fpath') });
			})
			.on('contextmenu', '.MyFileItem', function (e) {
				e.preventDefault();
				menuPath = $(this).attr('fpath');
				var isFolder = $(this).hasClass('MyFolderItem');
				if (isFolder)
					clearSelection();
				else if (selected.indexOf(menuPath) < 0)
					select(menuPath, false, false);
				showMenu(e.clientX, e.clientY, isFolder);
			});

		// A preview that cannot be shown falls back rather than leaving a broken image. Error events do not bubble.
		document.getElementById('MyFilesList').addEventListener('error', function (e) {
			var img = $(e.target);
			if (img.hasClass('MyFolderPreview'))
				img.remove();
			else if (img.hasClass('MyFilePreview') && img.attr('src') !== PLACEHOLDER)
				img.attr('src', PLACEHOLDER);
		}, true);

		$('#MyFilesBreadcrumb').on('click', '.MyFilesCrumb', function () { openFolder($(this).attr('frel')); });
		$('#MyFilesUpBtn').on('click', goUp);
		$('#MyFilesRefreshBtn').on('click', refresh);
		$('#MyFilesChangeBtn').on('click', function () { send('myfiles_change_folder'); });
		$('#MyFilesChooseBtn').on('click', function () { send('myfiles_select_folder'); });
		$('#MyFilesDeleteBtn, #MyFilesMenuDelete').on('click', function () { actionPaths('myfiles_delete_files'); });
		$('#MyFilesMoveBtn, #MyFilesMenuMove').on('click', function () { actionPaths('myfiles_move_files'); });
		$('#MyFilesClearBtn').on('click', clearSelection);
		$('#MyFilesMenuExplore').on('click', function () {
			hideMenu();
			send('myfiles_explore_file', { path: menuPath });
		});
		$('#MyFilesRecursive').on('change', function () { send('myfiles_set_recursive', { recursive: this.checked }); });
		$('#MyFilesSort').on('change', function () {
			if (SORT_MODES.indexOf(this.value) < 0)
				return;
			sortMode = this.value;
			try { localStorage.setItem(SORT_KEY, sortMode); } catch (e) {}
			renderList();
		});

		$('#MyFilesBoard').on('contextmenu', function (e) { e.preventDefault(); }).on('scroll', hideMenu);
		$(document).on('mousedown.myfiles', function (e) {
			if (!$(e.target).closest('#MyFilesMenu').length)
				hideMenu();
		});
		$(document).on('keydown.myfiles', onKey);
	}

	return {
		init: function () {
			try {
				var saved = localStorage.getItem(SORT_KEY);
				if (SORT_MODES.indexOf(saved) >= 0)
					sortMode = saved;
			} catch (e) {}
			$('#MyFilesSort').val(sortMode);
			$('#MyFilesHintText').text(isMac ?
				T('myfiles_hint_mac', 'Double-click to open a file or folder. ⌘+click or Shift+click to select several files. Backspace goes up a folder.') :
				T('myfiles_hint', 'Double-click to open a file or folder. Ctrl+click or Shift+click to select several files. Backspace goes up a folder.'));
			bind();
		},

		// Called on every visit, so changes made outside the app show up
		show: refresh,

		hide: function () {
			hideMenu();
			clearSelection();
		},

		handle: function (message) {
			switch (message && message.command) {
			case 'get_my_files':
				showList(message);
				return true;
			case 'my_files_thumbnail':
				showPreview(message.path, message.image);
				showProgress(message.done, message.total);
				return true;
			case 'my_files_thumbnail_progress':
				showProgress(message.done, message.total);
				return true;
			}
			return false;
		}
	};
})();
