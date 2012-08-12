#include <curl/curl.h>
#include <curl/easy.h>

#include "../SDK/foobar2000.h"
#include "../helpers/helpers.h"
#include "../utf8api/utf8api.h"
#include "resource.h"
#include <commctrl.h>

#include <map>
#include <vector>
#include <fstream>
#include <iostream>
#include <algorithm>

static cfg_string cfg_username("username", "");
static cfg_string cfg_password("password", "");

#define POST_URL "http://example.com/index-upload-handler"
#define DOMAIN   "example.com"

class Track
{
public:
	string8 type;
	string8 artist;
	string8 album;
	string8 profile;
	int bitrate;
	int length;
	__int64 filesize;
};

int trackcmp(const Track *&a, const Track *&b)
{
	if (a == NULL || b == NULL || a->artist.get_ptr() == NULL || a->album.get_ptr() == NULL || b->artist.get_ptr() == NULL || b->album.get_ptr() == NULL)
		return 0;

	int artist = stricmp_utf8_ex(a->artist.get_ptr(), a->artist.length(), b->artist.get_ptr(), b->artist.length());
	int album = stricmp_utf8_ex(a->album.get_ptr(), a->album.length(), b->album.get_ptr(), b->album.length());
	
	if (artist != 0)
		return artist;
	else if (album != 0)
		return album;
	else
		return 0;
}

static HWND waitingdlg = NULL;
static DWORD threadid = NULL;

struct progress_cb_struct
{
	HWND progress;
	HWND dialog;
	bool finished;
};

int waiting_progress_callback(void *ptr, double dltotal, double dlnow, double ultotal, double ulnow)
{
	struct progress_cb_struct* progress = (struct progress_cb_struct*)ptr;

	if (ulnow != 0 && ultotal != 0)
	{
		int percent = (ulnow / ultotal) * 100;
		uSendMessage(progress->progress, PBM_SETPOS, percent, 0);

		if (percent >= 100 && progress->finished != true)
		{
			uSetDlgItemText(waitingdlg, IDC_STATUSTEXT, string8("Status: Waiting on database..."));
			progress->finished = true;
		}
	} else {
		if (progress->finished != true)
		{
			uSetDlgItemText(waitingdlg, IDC_STATUSTEXT, string8("Status: Waiting on database..."));
			progress->finished = true;
		}
	}
	
	return 0;
}

DWORD WINAPI mSpiderThread(void *ptr)
{
	metadb* db = metadb::get();

	metadb_handle_list list;
	db->get_all_entries(list);

	char message[100];
	sprintf(message, "Status: Got %d items from MetaDB; sorting", list.get_count());
	uSetDlgItemText(waitingdlg, IDC_STATUSTEXT, string8(message));

	ptr_list_t<Track> tracks;

	for (size_t i = 0; i < list.get_count(); i++)
	{
		metadb_handle *first = list.get_item(i);
		
		Track *t = new Track();
		string8 tmp;
		first->handle_format_title(tmp, "%__bitrate%", NULL);
		t->bitrate = atoi(tmp.get_ptr());
		t->length = (int) first->handle_get_length();
		t->filesize = first->handle_get_file_size();
		first->handle_format_title(t->type, "$upper($ext(%_filename_ext%))", NULL);
		first->handle_format_title(t->album, "%album%", NULL);
		first->handle_format_title(t->artist, "%artist%", NULL);
		first->handle_format_title(t->profile, "$if(%__lame_profile%,-%__lame_profile%,)", NULL);

		if ((stricmp_utf8(string8("?"), t->album) != 0) && (stricmp_utf8(string8("?"), t->artist) != 0))
			tracks.add_item(t);
	}

	tracks.sort(&trackcmp);

	std::ofstream s("output.db", std::ofstream::binary);
	
	if (tracks.get_count() == 0)
	{
		Sleep(1500);

		EndDialog(waitingdlg,0);
		modeless_dialog_manager::remove(waitingdlg);

		list.delete_all();
		s.close();

		waitingdlg = NULL;
		threadid = NULL;

		return 0;
	}

	string8 cur_artist = string8(tracks.get_item(0)->artist);
	string8 cur_album = string8(tracks.get_item(0)->album);
	string8 cur_type = string8(tracks.get_item(0)->type);
	string8 cur_profile = string8(tracks.get_item(0)->profile);
	unsigned int cur_avgbitrate = 0;
	unsigned int cur_length = 0;
	size_t cur_count = 0;
	size_t album_count = 0;
	
	for (size_t i = 0; i < tracks.get_count(); i++)
	{
		Track *t = tracks.get_item(i);

		if ((stricmp_utf8(t->artist, cur_artist) != 0) || (stricmp_utf8(t->album, cur_album) != 0))
		{
			// write line
			if (cur_count != 0)
				cur_avgbitrate /= cur_count;

			s << strlen(cur_artist.get_ptr()) << ":'" << cur_artist << "'," << strlen(cur_album.get_ptr()) << ":'" << cur_album << "'," << strlen(cur_type.get_ptr()) << ":'" << cur_type << "'," << strlen(cur_profile.get_ptr()) << ":'" << cur_profile << "'," << cur_count << "," << cur_avgbitrate << "," << cur_length << "\n";
			album_count++;
			
			cur_artist = t->artist;
			cur_album = t->album;
			cur_profile = t->profile;
			cur_count = 1;
			cur_length = t->length;
			cur_avgbitrate = t->bitrate;
			cur_type = t->type;
		} else {
			cur_count++;
			cur_length += t->length;
			cur_avgbitrate += t->bitrate;
		}
	}

	sprintf(message, "Status: Collated into %d albums", album_count);
	uSetDlgItemText(waitingdlg, IDC_STATUSTEXT, string8(message));

	Sleep(1500);

	sprintf(message, "Status: Uploading to " DOMAIN);
	uSetDlgItemText(waitingdlg, IDC_STATUSTEXT, string8(message));

	list.delete_all();
	s.close();

	progress_cb_struct pcb;
	pcb.progress = GetDlgItem(waitingdlg, IDC_PROGRESS);
	pcb.dialog = waitingdlg;
	pcb.finished = false;

	/* now post to server */
	char url[] = POST_URL;

	/* init curl */
	curl_global_init(CURL_GLOBAL_ALL);
	CURL *curl = curl_easy_init();

	curl_easy_setopt(curl, CURLOPT_POST, 1);

	/* create post form */
	struct curl_httppost* post = NULL;
	struct curl_httppost* last = NULL;

	curl_formadd(&post, &last, CURLFORM_COPYNAME, "id",
		CURLFORM_COPYCONTENTS, cfg_username.get_val(), CURLFORM_END);

	curl_formadd(&post, &last, CURLFORM_COPYNAME, "pass",
		CURLFORM_COPYCONTENTS, cfg_password.get_val(), CURLFORM_END);

	curl_formadd(&post, &last, CURLFORM_COPYNAME, "data",
		CURLFORM_FILE, "output.db", CURLFORM_END);

	curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &pcb);
	curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, waiting_progress_callback);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
	
	/* this writes whatever the server says to stdout, so the php script will
	* write whatever happened here */
	curl_easy_perform(curl);
	curl_formfree(post);
	curl_easy_cleanup(curl);

	strcpy(message, "Status: Finished!");
	uSetDlgItemText(waitingdlg, IDC_STATUSTEXT, string8(message));
	uSendMessage(waitingdlg, WM_PAINT, 0, 0);

	Sleep(1500);

	EndDialog(waitingdlg,0);
	modeless_dialog_manager::remove(waitingdlg);

	waitingdlg = NULL;
	threadid = NULL;
	return 0;
}

static BOOL CALLBACK WaitingDialogProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		waitingdlg = wnd;
		break;
	case WM_DESTROY:
		modeless_dialog_manager::remove(waitingdlg);
		break;
	}

	return 0;
}

class components_menu_item_mspider : public menu_item_main_single
{
public:
	virtual void get_name(string_base & out)
	{
		out = "Components/mSpider/Upload List";
	}

	virtual void run()
	{
		if (cfg_username.is_empty() || cfg_password.is_empty())
		{
			play_control::g_show_config("mSpider");
			return;
		}

		if (threadid != NULL)
		{
			uMessageBox(core_api::get_main_window(), "An upload is already in progress.  Please wait.", "mSpider Warning",0);
			return;
		}

		uCreateDialog(IDD_HANGON, core_api::get_main_window(), WaitingDialogProc);
	
		if (waitingdlg)
		{
			modeless_dialog_manager::add(waitingdlg);
			ShowWindow(waitingdlg, SW_SHOW);
			SetForegroundWindow(waitingdlg);

			CreateThread(NULL, 0, mSpiderThread, NULL, 0, &threadid);
		}
	}
};

class config_mspider : public config
{
	static BOOL CALLBACK ConfigProc(HWND wnd,UINT msg,WPARAM wp,LPARAM lp)
	{
		switch(msg)
		{
		case WM_INITDIALOG:
			uSetDlgItemText(wnd, IDC_USERNAME, cfg_username);
			uSetDlgItemText(wnd, IDC_PASSWORD, cfg_password);
			break;
		case WM_COMMAND:
			switch (wp)
			{
			case (EN_CHANGE << 16) | IDC_USERNAME:
				cfg_username = string_utf8_from_window((HWND)lp);
				break;
			case (EN_CHANGE << 16) | IDC_PASSWORD:
				cfg_password = string_utf8_from_window((HWND)lp);
				break;
			}
		case WM_DESTROY:
			break;
		}
		return 0;
	}
public:
	virtual HWND create(HWND parent)
	{
		return uCreateDialog(IDD_CONFIG,parent,ConfigProc);
	}
	virtual const char * get_name() {return "mSpider";}
	virtual const char * get_parent_name() {return "Components";}
};

class initquit_mspider : public initquit
{
public:
	virtual void on_init()
	{

	}

	virtual void on_quit()
	{
		if (waitingdlg != NULL)
		{
			modeless_dialog_manager::remove(waitingdlg);
			waitingdlg = NULL;
		}
	}
};



static service_factory_single_t<initquit,initquit_mspider> myinitquitlistener;
static service_factory_single_t<menu_item,components_menu_item_mspider> mymenu;
static service_factory_single_t<config, config_mspider> myconfig;