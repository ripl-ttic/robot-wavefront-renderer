#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <lcm/lcm.h>

#include <bot_core/bot_core.h>
#include <bot_vis/bot_vis.h>
#include <bot_frames/bot_frames.h>

#include <lcmtypes/bot_core_pose_t.h>

#define RENDERER_NAME "Robot"

#define PARAM_BLING "Bling"
#define PARAM_SHOW_SHADOW "Shadow"


#define DETAIL_NONE 0
#define DETAIL_SPEED 1
#define DETAIL_RPY 2
#define NUM_DETAILS 3

#define SQ(x) ((x)*(x))
#define TO_DEG(x) ((x)*180.0/M_PI)


typedef struct _RendererRobot {
    BotRenderer renderer;
    BotEventHandler ehandler;

    lcm_t *lcm;
    BotParam * param;
    BotFrames * frames;

    BotWavefrontModel *robot_model;
    BotViewer *viewer;
    BotGtkParamWidget *pw;

    bot_core_pose_t *bot_pose_last;

    double footprint[8];

    const char * draw_frame;
    const char * model_param_prefix;

    int display_lists_ready;
    int display_detail;
    GLuint robot_dl;
} RendererRobot;


static void
on_bot_pose (const lcm_recv_buf_t *buf, const char *channel,
             const bot_core_pose_t *msg, void *user) {

    RendererRobot *self = (RendererRobot *)user;
    if (self->bot_pose_last)
        bot_core_pose_t_destroy (self->bot_pose_last);
    self->bot_pose_last = bot_core_pose_t_copy (msg);
}

static void
draw_wavefront_model (RendererRobot * self)
{
    glEnable (GL_BLEND);
    glEnable (GL_RESCALE_NORMAL);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glShadeModel (GL_SMOOTH);
    glEnable (GL_LIGHTING);
    glCallList (self->robot_dl);
}

static void
frames_update_handler(BotFrames *bot_frames, const char *frame, const char * relative_to, int64_t utime,
                                  void *user)
{
    RendererRobot *self = (RendererRobot *) user;
    if (strcmp(frame, "body") == 0)
        bot_viewer_request_redraw(self->viewer);
}

static void
on_find_button(GtkWidget *button, RendererRobot *self)
{
    BotViewHandler *vhandler = self->viewer->view_handler;

    double eye[3];
    double lookat[3];
    double up[3];

    vhandler->get_eye_look(vhandler, eye, lookat, up);
    double diff[3];
    bot_vector_subtract_3d(eye, lookat, diff);

    BotTrans pose;
    bot_frames_get_trans(self->frames, "body", self->draw_frame, &pose);

    bot_vector_add_3d(pose.trans_vec, diff, eye);

    vhandler->set_look_at(vhandler, eye, pose.trans_vec, up);

    bot_viewer_request_redraw(self->viewer);
}

static void
robot_free(BotRenderer *super)
{
    RendererRobot *self = (RendererRobot*) super->user;

    if (self->robot_model)
        bot_wavefront_model_destroy(self->robot_model);
    free(self);
}

static GLuint
compile_display_list (RendererRobot * self, BotWavefrontModel * model)
{
    GLuint dl = glGenLists (1);
    glNewList (dl, GL_COMPILE);

    const char * prefix = self->model_param_prefix;
    char key[1024];

    glPushMatrix();

    sprintf(key, "%s.translate", prefix);
    double trans[3];
    if (bot_param_get_double_array(self->param, key, trans, 3) == 3)
        glTranslated(trans[0], trans[1], trans[2]);

    sprintf(key, "%s.scale", prefix);
    double scale;
    if (bot_param_get_double(self->param, key, &scale) == 0)
        glScalef(scale, scale, scale);

    sprintf(key, "%s.rotate_xyz", prefix);
    double rot[3];
    if (bot_param_get_double_array(self->param, key, rot, 3) == 3) {
        glRotatef(rot[2], 0, 0, 1);
        glRotatef(rot[1], 0, 1, 0);
        glRotatef(rot[0], 1, 0, 0);
    }

    glEnable(GL_LIGHTING);
    bot_wavefront_model_gl_draw(model);
    glDisable(GL_LIGHTING);

    glPopMatrix();

    glEndList ();
    return dl;
}

static void
draw_footprint (RendererRobot *self)
{
    glPushAttrib (GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable (GL_DEPTH_TEST);
    glLineWidth(2);
    glColor4f (1, 1, 0, .5);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset (1, 1);

    glBegin (GL_QUADS);
    for (int i=0; i<4; i++)
        glVertex2dv (self->footprint + 2*i);
    glEnd ();

    glColor4f (0, 1, 0, 1);
    glBegin (GL_LINE_LOOP);
    for (int i=0; i<4; i++)
        glVertex2dv (self->footprint + 2*i);
    glEnd();
    glDisable(GL_POLYGON_OFFSET_FILL);

    double fp_length = self->footprint[0] - self->footprint[4];
    double fp_width = fabs (self->footprint[1] - self->footprint[3]);

    glPushMatrix ();
    glTranslatef (self->footprint[0] - fp_length/2,
            self->footprint[1] - fp_width/2, 0);
    bot_gl_draw_arrow_2d (fp_length, fp_width, fp_length * 0.3,
            fp_width * 0.5, self->ehandler.hovering);
    glPopMatrix ();
    glPopAttrib ();
}


static void
robot_draw(BotViewer *viewer, BotRenderer *super)
{
    RendererRobot *self = (RendererRobot*) super->user;

    if (!bot_frames_have_trans(self->frames, "body", self->draw_frame))
        return;

    int bling = bot_gtk_param_widget_get_bool(self->pw, PARAM_BLING);
    if (bling && self->robot_model && !self->display_lists_ready) {
        self->robot_dl = compile_display_list(self, self->robot_model);
        self->display_lists_ready = 1;
    }

    // get the transform to orient the vehicle in drawing coordinates
    BotTrans body_to_local;
    bot_frames_get_trans(self->frames, "body", self->draw_frame, &body_to_local);

    //if (bling && self->display_lists_ready) {
    double body_to_local_m[16], body_to_local_m_opengl[16];

    glEnable(GL_DEPTH_TEST);

    bot_trans_get_mat_4x4(&body_to_local, body_to_local_m);

    // opengl expects column-major matrices
    bot_matrix_transpose_4x4d(body_to_local_m, body_to_local_m_opengl);
    glPushMatrix();
    glMultMatrixd(body_to_local_m_opengl); // rotate and translate the vehicle

    if (bling && self->display_lists_ready)
        draw_wavefront_model(self);
    else
        draw_footprint (self);


    glPopMatrix();


    if (self->viewer && self->display_detail && self->bot_pose_last) {
        char buf[256];
        switch (self->display_detail) {
        case DETAIL_SPEED: {

            double vel_body[3];
            double rate_body[3];

            bot_frames_rotate_vec (self->frames, "local", "body", self->bot_pose_last->vel, vel_body);

            bot_frames_rotate_vec (self->frames, "local", "body", self->bot_pose_last->rotation_rate, rate_body);

            sprintf(buf, "tv: %.2f m/s\nrv: %.2f deg/s", vel_body[0], rate_body[2] * 180/M_PI);
        }
        case DETAIL_RPY: {
            double rpy[3];
            bot_quat_to_roll_pitch_yaw(self->bot_pose_last->orientation, rpy);
            sprintf(buf, "r: %6.2f\np: %6.2f\ny: %6.2f", TO_DEG(rpy[0]), TO_DEG(rpy[1]), TO_DEG(rpy[2]));
            break;
            }
        }
        glColor3f(1,1,1);
        bot_gl_draw_text(body_to_local.trans_vec, GLUT_BITMAP_HELVETICA_12, buf,
                         BOT_GL_DRAW_TEXT_DROP_SHADOW);
    }



    if (bot_gtk_param_widget_get_bool(self->pw, PARAM_SHOW_SHADOW)) {
        glColor4f(0, 0, 0, 0.2);
        glPushMatrix();
        glTranslated(body_to_local.trans_vec[0], body_to_local.trans_vec[1], 0);
        glTranslated(-0.04, 0.03, 0);
        double rpy[3];
        bot_quat_to_roll_pitch_yaw(body_to_local.rot_quat, rpy);
        glRotatef(rpy[2] * 180 / M_PI + 45, 0, 0, 1);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        glVertex2f(0.3, -0.3);
        glVertex2f(0.3, 0.3);
        glVertex2f(-0.3, 0.3);
        glVertex2f(-0.3, -0.3);
        glEnd();
        glPopMatrix();
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
    }
}


static int
mouse_press (BotViewer *viewer, BotEventHandler *ehandler,
             const double ray_start[3], const double ray_dir[3],
             const GdkEventButton *event)
{
    RendererRobot *self = (RendererRobot*) ehandler->user;

    if (event->type == GDK_2BUTTON_PRESS) {
        self->display_detail = (self->display_detail + 1) % NUM_DETAILS;
        bot_viewer_request_redraw(self->viewer);
    }

    return 0;
}

static void
on_param_widget_changed(BotGtkParamWidget *pw, const char *name, void *user)
{
    RendererRobot *self = (RendererRobot*) user;
    bot_viewer_request_redraw(self->viewer);
}

static void
on_load_preferences(BotViewer *viewer, GKeyFile *keyfile, void *user_data)
{
    RendererRobot *self = (RendererRobot *) user_data;
    bot_gtk_param_widget_load_from_key_file(self->pw, keyfile, RENDERER_NAME);
}

static void
on_save_preferences(BotViewer *viewer, GKeyFile *keyfile, void *user_data)
{
    RendererRobot *self = (RendererRobot *) user_data;
    bot_gtk_param_widget_save_to_key_file(self->pw, keyfile, RENDERER_NAME);
}

void
setup_renderer_robot_model(BotViewer *viewer, int render_priority,
                           BotParam * param, BotFrames * frames)
{
    RendererRobot *self = (RendererRobot*) calloc(1, sizeof(RendererRobot));

    self->viewer = viewer;
    BotRenderer *renderer = &self->renderer;
    self->lcm = bot_lcm_get_global (NULL);

    renderer->draw = robot_draw;
    renderer->destroy = robot_free;

    renderer->widget = gtk_vbox_new(FALSE, 0);
    renderer->name = (char *) RENDERER_NAME;
    renderer->user = self;
    renderer->enabled = 1;

    BotEventHandler *ehandler = &self->ehandler;
    ehandler->name = (char *) RENDERER_NAME;
    ehandler->enabled = 1;
    ehandler->pick_query = NULL;
    ehandler->key_press = NULL;
    ehandler->hover_query = NULL;
    ehandler->mouse_press = mouse_press;
    ehandler->mouse_release = NULL;
    ehandler->mouse_motion = NULL;
    ehandler->user = self;

    /* attempt to load wavefront model files */
    self->param = param;
    self->frames = frames;
    bot_frames_add_update_subscriber(self->frames, frames_update_handler, (void *) self);

    self->draw_frame = bot_frames_get_root_name(self->frames);

    const char * models_dir = BASE_PATH "/models";

    char *model_name;
    char model_full_path[256];
    self->model_param_prefix = "models.robot";
    char param_key[1024];
    snprintf(param_key, sizeof(param_key), "%s.wavefront_model", self->model_param_prefix);

    int footprint_only = 0;
    if (bot_param_get_str(self->param, param_key, &model_name) == 0) {
        snprintf(model_full_path, sizeof(model_full_path), "%s/%s", models_dir, model_name);
        self->robot_model = bot_wavefront_model_create(model_full_path);
        double minv[3];
        double maxv[3];
        bot_wavefront_model_get_extrema(self->robot_model, minv, maxv);

        double span_x = maxv[0] - minv[0];
        double span_y = maxv[1] - minv[1];
        double span_z = maxv[2] - minv[2];

        double span_max = MAX(span_x, MAX(span_y, span_z));

        //printf("WAVEFRONT extrema: [%f, %f, %f] [%f, %f, %f]\n",
        //       minv[0], minv[1], minv[2],
        //       maxv[0], maxv[1], maxv[2]);

    }
    else {
        footprint_only = 1;
        fprintf(stderr, "Robot model name not found under param %s, drawing footprint only\n", param_key);
    }


    // Get the vehicle footprint
    double *fp = self->footprint;
    bot_param_get_double_array_or_fail (self->param, "calibration.vehicle_bounds.front_left",
                                        fp, 2);
    bot_param_get_double_array_or_fail (self->param, "calibration.vehicle_bounds.front_right",
                                        fp+2, 2);
    bot_param_get_double_array_or_fail (self->param, "calibration.vehicle_bounds.rear_right",
                                        fp+4, 2);
    bot_param_get_double_array_or_fail (self->param, "calibration.vehicle_bounds.rear_left",
                                        fp+6, 2);

    self->pw = BOT_GTK_PARAM_WIDGET(bot_gtk_param_widget_new());
    gtk_box_pack_start(GTK_BOX(renderer->widget), GTK_WIDGET(self->pw), TRUE, TRUE, 0);

    bot_gtk_param_widget_add_booleans(self->pw, BOT_GTK_PARAM_WIDGET_CHECKBOX,
                                      PARAM_BLING, 1, PARAM_SHOW_SHADOW, 0, NULL);


    GtkWidget *find_button = gtk_button_new_with_label("Find");
    gtk_box_pack_start(GTK_BOX(renderer->widget), find_button, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(find_button), "clicked", G_CALLBACK(on_find_button), self);

    gtk_widget_show_all(renderer->widget);

    g_signal_connect(G_OBJECT(self->pw), "changed", G_CALLBACK(on_param_widget_changed), self);
    on_param_widget_changed(self->pw, "", self);

    //self->bot_pose_last = NULL;
    bot_core_pose_t_subscribe (self->lcm, "POSE", on_bot_pose, self);
    bot_viewer_add_event_handler(viewer, &self->ehandler, render_priority);
    bot_viewer_add_renderer(viewer, &self->renderer, render_priority);

    g_signal_connect(G_OBJECT(viewer), "load-preferences", G_CALLBACK(on_load_preferences), self);
    g_signal_connect(G_OBJECT(viewer), "save-preferences", G_CALLBACK(on_save_preferences), self);

    if (footprint_only)
        bot_gtk_param_widget_set_enabled (self->pw, PARAM_BLING, 0);
}
